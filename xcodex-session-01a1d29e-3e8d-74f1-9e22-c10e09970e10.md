# Codex conversation

## User

привет, можешь продолжить то над чем мы работали? вот наша с тобой предыдущая сессия /mnt/media/machoman/codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md

## Assistant

Привет! Прочитаю предыдущую сессию и проверю состояние проекта, чтобы продолжить с того места, где мы остановились.

## Activity

    $ wc -l codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md && tail -n 180 codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md
    24835 codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md
        selection callback
        selection callback
        selection callback
        selection callback
        selection callback
        selection callback
        selection callback
        selection callback
        window destructor
        run: entry returned x0=0
        host signal stop
        #0  0x00007ffff7fc8f35 in ?? () from /lib64/ld-linux-x86-64.so.2
        #1  0x00007ffff7fc9b16 in ?? () from /lib64/ld-linux-x86-64.so.2
        #2  0x00007ffff7972376 in ?? () from /usr/lib/libc.so.6
        #3  0x00007ffff78945d0 in ?? () from /usr/lib/libc.so.6
        #4  0x00007ffff7fc0479 in _dl_catch_exception () from /lib64/ld-linux-x86-64.so.2
        #5  0x00007ffff7fc05c9 in ?? () from /lib64/ld-linux-x86-64.so.2
        #6  0x00007ffff7893fa3 in ?? () from /usr/lib/libc.so.6
        #7  0x00007ffff789466e in dlsym () from /usr/lib/libc.so.6
        #8  0x00005555555ff91f in (anonymous namespace)::guest_process::~guest_process (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1254
        #9  0x00005555555fbeca in mm_runtime_launch (request=...) at /mnt/media/machoman/src/runtime/loader.cpp:2644
        #10 0x0000555555564a72 in (anonymous namespace)::cmd_run (venv_dir=..., binary=..., args=..., env=..., apply_defaults=..., signal_mask=...) at /mnt/media/machoman/src/cli/main.cpp:296
        #11 0x00005555555612e5 in main (argc=..., argv=...) at /mnt/media/machoman/src/cli/main.cpp:451
        #0  0x00007ffff7fc8f35 in ?? () from /lib64/ld-linux-x86-64.so.2
        => 0x7ffff7fc8f35:    mov    0x28(%rax),%rbx
           0x7ffff7fc8f39:    cmp    %r11,%rbx
           0x7ffff7fc8f3c:    je     0x7ffff7fc8f23
           0x7ffff7fc8f3e:    test   $0x2,%r15b
           0x7ffff7fc8f42:    je     0x7ffff7fc8f4d
        rax            0x0                 0
        rbx            0x555555686673      93824993486451
        rcx            0x0                 0
        rdx            0x7fffffffc978      140737488341368
        rsi            0x92f0e82a          2465261610
        rdi            0x555555686673      93824993486451
        r8             0x3                 3
        r9             0x555555726700      93824994141952
        #8  0x00005555555ff91f in (anonymous namespace)::guest_process::~guest_process (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1254
        1254                    auto stop = reinterpret_cast<void (*)()>(dlsym(library->handle, name));
        $1 = "libncursesw.so.6"
        $2 = (void *) 0x555555726700
        A debugging session is active.
    
            Inferior 1 [process 218689] will be killed.
    
        Quit anyway? (y or n) [answered Y; input not from terminal]
        ✓ • 735ms
    
    ## Activity
    
        $ sed -n '1540,1585p' src/runtime/loader.cpp
            rg -n 'native_x\[MM_RSP\]|native_entry|native_mode' src/runtime/loader.cpp | head -n 35
            sed -n '155,195p' src/dylibs/libobjc/native_abi.h
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
        44:extern "C" { thread_local std::uint64_t mm_native_entry_pc = 0; }
        49:              offsetof(mm_cpu_context, native_mode) == 1048 && offsetof(mm_cpu_context, native_v) == 1056,
        1588:        t.ctx.native_mode = 1;
        1592:        t.ctx.native_x[MM_RSP] = t.ctx.x[31] - 8;
        1594:        std::memcpy(reinterpret_cast<void *>(t.ctx.native_x[MM_RSP]), initial_stack, sizeof initial_stack);
        1627:                t.ctx.x[31] = t.ctx.native_x[MM_RSP];
        1633:            const auto sp = t.ctx.native_x[MM_RSP];
        1796:                t.ctx.native_x[MM_RSP] += 8;
        2044:    if (t.ctx.native_mode) {
        2080:    if (t.ctx.native_mode) {
        2081:        const auto sp = ((t.ctx.native_x[MM_RSP] - 1024) & ~std::uint64_t{15}) - 8;
        2088:        t.ctx.native_x[MM_RSP] = sp;
        2187:    if (t->ctx.native_mode) t->ctx.native_x[MM_RDX] = native_rdx;
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
        ✓ • 0ms
    
    ## Activity
    
        file changes: Completed · 1 changes
        Update { move_path: None }: /mnt/media/machoman/src/runtime/loader.cpp
        @@ -2159,3 +2159,7 @@
             for (int i = 0; i < 8; i++) t->ctx.x[i] = i < nargs ? args[i] : 0;
        -    t->ctx.x[31] = (saved.x[31] - 256) & ~std::uint64_t{15};
        +    /* An Intel aggregate adapter temporarily uses x[31] for flattened
        +     * host arguments, which may live in a small heap vector. Nested guest
        +     * callbacks must keep using the real Intel stack, below its red zone. */
        +    const auto callback_sp = saved.native_mode ? saved.native_x[MM_RSP] : saved.x[31];
        +    t->ctx.x[31] = (callback_sp - 256) & ~std::uint64_t{15};
             if (nargs > 8) {
    
    ## Activity
    
        $ cmake --build /tmp/machoman-roblox-input-build --target machoman -j 4
        [1/3] Building CXX object CMakeFiles/machoman_core.dir/src/runtime/loader.cpp.o
        /mnt/media/machoman/src/runtime/loader.cpp:2563:122: warning: missing field 'dependency' initializer [-Wmissing-designated-field-initializers]
         2563 |                                         .resolution = mm_import_resolution::missing, .lazy = bind.lazy, .weak = bind.weak};
              |                                                                                                                          ^
        1 warning generated.
        [2/3] Linking CXX static library libmachoman_core.a
        [3/3] Linking CXX executable machoman
        ✓ • 5.35s
    ✓ • 0ms

## Activity

    $ git status --short && rg --files -g AGENTS.md -g CLAUDE.md
     M .gitignore
     M CMakeLists.txt
     M README.md
     M include/machoman/runtime.h
     M include/machoman/translator.h
     M include/machoman/venv.h
     M modules/MEVK/CMakeLists.txt
     M modules/MEVK/README.md
     M modules/MEVK/include/mevk/command_buffer.h
     M modules/MEVK/include/mevk/context.h
     M modules/MEVK/include/mevk/render.h
     M modules/MEVK/include/mevk/shader.h
     M modules/MEVK/src/metal/CMakeLists.txt
     M modules/MEVK/src/metal/coregraphics_metal.cpp
     M modules/MEVK/src/metal/metal.cpp
     M modules/MEVK/src/metal/metal_internal.h
     M modules/MEVK/src/metal/render_metal.cpp
     M modules/MEVK/src/shader/air.cpp
     M modules/MEVK/src/vulkan/buffer.cpp
     M modules/MEVK/src/vulkan/command_buffer.cpp
     M modules/MEVK/src/vulkan/context.cpp
     M modules/MEVK/src/vulkan/internal.h
     M modules/MEVK/src/vulkan/render.cpp
     M modules/MEVK/src/vulkan/texture.cpp
     M modules/MEVK/tests/CMakeLists.txt
     M modules/MEVK/tests/metal/CMakeLists.txt
     M modules/MEVK/tests/metal/guest_metal_render.m
     M modules/MEVK/tests/metal/guest_metal_smoke.m
     M modules/MEVK/tests/test_render.cpp
     M modules/MEVK/tests/test_shaders.cpp
     M modules/MEVK/tools/mevk_shader_inspect.cpp
     M modules/evilcocoa/README.md
     M modules/evilcocoa/include/evilcocoa/renderer.h
     M modules/evilcocoa/src/renderer.cpp
     M modules/evilcocoa/tests/test_renderer.cpp
     M scripts/build-guest-runtimes.sh
     M src/cli/main.cpp
     M src/dylibs/AppKit/appkit.h
     M src/dylibs/AppKit/appkit_main.cpp
     M src/dylibs/AppKit/application.cpp
     M src/dylibs/AppKit/controls.cpp
     M src/dylibs/AppKit/evilcocoa_adapter.cpp
     M src/dylibs/AppKit/nib.cpp
     M src/dylibs/AppKit/nib_archive.cpp
     M src/dylibs/AppKit/nib_archive.h
     M src/dylibs/AppKit/opengl.cpp
     M src/dylibs/AppKit/platform_headless.cpp
     M src/dylibs/AppKit/platform_wayland.cpp
     M src/dylibs/AppKit/responder.cpp
     M src/dylibs/AppKit/views.cpp
     M src/dylibs/Foundation/collections.cpp
     M src/dylibs/Foundation/dispatch.cpp
     M src/dylibs/Foundation/formatting.cpp
     M src/dylibs/Foundation/foundation.h
     M src/dylibs/Foundation/misc.cpp
     M src/dylibs/Foundation/plists.cpp
     M src/dylibs/Foundation/strings.cpp
     M src/dylibs/Foundation/task.cpp
     M src/dylibs/Foundation/threading.cpp
     M src/dylibs/carbon/carbon.cpp
     M src/dylibs/libSystem/callbacks.cpp
     M src/dylibs/libSystem/internal.h
     M src/dylibs/libSystem/kqueue.cpp
     M src/dylibs/libSystem/libSystem.cpp
     M src/dylibs/libSystem/socket.cpp
     M src/dylibs/libSystem/stdio.cpp
     M src/dylibs/libSystem/xlocale.cpp
     M src/dylibs/libobjc/objc.cpp
     M src/dylibs/libobjc/stub_abi.h
     M src/dylibs/symbol-synthesis/file_types.h
     M src/runtime/hostcall_x86_64.S
     M src/runtime/loader.cpp
     M src/venv/venv.cpp
     M tests/CMakeLists.txt
     M tests/guest_dialog_smoke.m
     M tests/test_nib_archive.cpp
     m third_party/llvm-project
    ?? codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md
    ?? include/machoman/environment.h
    ?? include/machoman/guest_args.h
    ?? include/machoman/host_environment.h
    ?? include/machoman/ipc.h
    ?? include/machoman/json.h
    ?? modules/MEVK/src/shader/cache.cpp
    ?? modules/MEVK/src/shader/cache.h
    ?? modules/MEVK/src/vulkan/compute.cpp
    ?? modules/MEVK/tests/metal/guest_display_geometry.m
    ?? modules/MEVK/tests/test_air_resources.cpp
    ?? modules/MEVK/tests/test_shader_cache.cpp
    ?? modules/libresolv/
    ?? scripts/relocate-homebrew-bottles.py
    ?? scripts/summarize-import-audit.py
    ?? src/common/json.cpp
    ?? src/dylibs/AppKit/clipboard.h
    ?? src/dylibs/AppKit/clipboard_wayland.cpp
    ?? src/dylibs/AppKit/clipboard_wayland.h
    ?? src/dylibs/AppKit/pasteboard.cpp
    ?? src/dylibs/AppKit/stack_view.cpp
    ?? src/dylibs/AppKit/text_view.cpp
    ?? src/dylibs/Foundation/cookies.cpp
    ?? src/dylibs/Foundation/dispatch_data.cpp
    ?? src/dylibs/Foundation/index_set.cpp
    ?? src/dylibs/libSystem/environment.cpp
    ?? src/dylibs/libSystem/mach_bootstrap.cpp
    ?? src/dylibs/libSystem/mach_host.cpp
    ?? src/dylibs/libSystem/mach_ipc.h
    ?? src/dylibs/libSystem/mach_memory.cpp
    ?? src/dylibs/libSystem/mach_messages.cpp
    ?? src/dylibs/libSystem/mach_ports.cpp
    ?? src/dylibs/libSystem/mach_tasks.cpp
    ?? src/dylibs/libSystem/passwd.cpp
    ?? src/dylibs/libSystem/proc_info.cpp
    ?? src/dylibs/libSystem/processes.cpp
    ?? src/dylibs/libSystem/shared_memory.cpp
    ?? src/dylibs/libSystem/sysinfo.cpp
    ?? src/dylibs/libSystem/ulock.cpp
    ?? src/dylibs/libSystem/uuid.cpp
    ?? src/dylibs/libSystem/xattr.cpp
    ?? src/dylibs/libobjc/native_abi.h
    ?? src/dylibs/libresolv/
    ?? src/ipc/
    ?? src/runtime/environment.cpp
    ?? src/runtime/native_x86_64.S
    ?? tests/environment_probe.cpp
    ?? tests/guest_atfork.c
    ?? tests/guest_dispatch_data.c
    ?? tests/guest_environment.m
    ?? tests/guest_errno.c
    ?? tests/guest_file_locks.c
    ?? tests/guest_file_urls.m
    ?? tests/guest_focus_reentrancy.m
    ?? tests/guest_http_cookies.m
    ?? tests/guest_import_audit.c
    ?? tests/guest_inet_addresses.c
    ?? tests/guest_locale_time.c
    ?? tests/guest_mach_exchange.c
    ?? tests/guest_mach_host.c
    ?? tests/guest_mach_notifications.c
    ?? tests/guest_mach_ports.c
    ?? tests/guest_memset_s.c
    ?? tests/guest_native_geometry.m
    ?? tests/guest_native_interpose.c
    ?? tests/guest_native_interposer.c
    ?? tests/guest_native_signals.c
    ?? tests/guest_native_smoke.c
    ?? tests/guest_notifications.m
    ?? tests/guest_opengl_threads.m
    ?? tests/guest_passwd.c
    ?? tests/guest_pasteboard.m
    ?? tests/guest_proc_info.c
    ?? tests/guest_process_shared.c
    ?? tests/guest_resource_limits.c
    ?? tests/guest_setjmp.c
    ?? tests/guest_string_format.m
    ?? tests/guest_text_view.m
    ?? tests/guest_thread_destructors.c
    ?? tests/guest_ulock.c
    ?? tests/guest_uname.c
    ?? tests/guest_xattr.c
    ?? tests/nib_lifetime_fixture.h
    ?? tests/prepare_environment_venv.cmake
    ?? tests/test_bottle_relocation.py
    ?? tests/test_environment.cpp
    ?? tests/test_import_audit.cpp
    ?? tests/test_ipc_kernel.cpp
    ?? tests/test_ipc_transport.cpp
    ?? tests/test_runtime_arch.cpp
    ?? tests/test_uuid.cpp
    ?? tests/test_wayland_clipboard.cpp
    AGENTS.md
    ✓ • 6.07s

## Activity

    $ rg -n '^## (User|Assistant)|^# |native|Intel|selection callback' codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md | tail -n 130
    21298:    selection callback
    21542:## User
    21546:## Assistant
    21764:## User
    21978:        rg -n 'native_mode|native_x|native_v' src/dylibs/AppKit/appkit_main.cpp src/dylibs/Foundation/formatting.cpp include/machoman/guest_args.h | head -n 45
    21996:    include/machoman/guest_args.h:30:        if (!ctx || !ctx->native_mode || !base) return;
    21997:    include/machoman/guest_args.h:49:        if (!ctx->native_mode) return result;
    21998:    include/machoman/guest_args.h:51:        result.p_ = ctx->native_x[MM_RSP] + 8;
    21999:    include/machoman/guest_args.h:55:        for (unsigned i = 0; i < 6; ++i) result.gpr_[i] = ctx->native_x[args[i]];
    22000:    include/machoman/guest_args.h:56:        for (unsigned i = 0; i < 8; ++i) result.fpr_[i] = ctx->native_v[i][0];
    22109:            if (!ctx || !ctx->native_mode || !base) return;
    22112:            native_ = true;
    22128:            if (!ctx->native_mode) return result;
    22129:            result.native_ = true;
    22130:            result.p_ = ctx->native_x[MM_RSP] + 8;
    22134:            for (unsigned i = 0; i < 6; ++i) result.gpr_[i] = ctx->native_x[args[i]];
    22135:            for (unsigned i = 0; i < 8; ++i) result.fpr_[i] = ctx->native_v[i][0];
    22140:            if (native_ && gp_ < 6) return gpr_[gp_++];
    22147:            if (native_ && fp_ < 8) bits = fpr_[fp_++];
    22152:            /* Darwin arm64 long double is double; Intel uses stack-passed x87
    22154:            if (!native_) return static_cast<long double>(next_double());
    22173:        bool native_ = false;
    22290:        rg -n 'native_geometry|native-geometry' tests/CMakeLists.txt
    22369:    360:                "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_geometry.m" -o "${MM_GEOMETRY_BINARY}"
    22370:    361:            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_geometry.m VERBATIM)
    22709:                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_geometry.m" -o "${MM_GEOMETRY_BINARY}"
    22710:                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_geometry.m VERBATIM)
    22742:    selection callback
    22743:    selection callback
    22744:    selection callback
    22745:    selection callback
    22746:    selection callback
    22747:    selection callback
    22748:    selection callback
    22749:    selection callback
    22750:    selection callback
    22751:    selection callback
    22752:    selection callback
    22837:## Assistant
    22839:Теперь есть объяснение и повтору текста, и кликам. Roblox очищает буфер методом `replaceCharactersInRange:withString:`, который оставался заглушкой. А при обработке мыши он вызывает `NSPointInRect`: наша версия неверно читает прямоугольник из Intel-кода, поэтому клики доходят до вида Roblox, но проверка координат их отбрасывает.
    22856:    +     * would incorrectly skip self/selector/format in the Intel save area. */
    22873:    +    /* Intel passes NSPoint in xmm0/xmm1 but the 32-byte NSRect on the
    22875:    +    if (auto *ctx = api()->current_context(); ctx && ctx->native_mode) {
    22877:    +        std::memcpy(rect, reinterpret_cast<const void *>(ctx->native_x[MM_RSP] + 8), sizeof rect);
    22896:    Update { move_path: None }: /mnt/media/machoman/tests/guest_native_geometry.m
    23135:        if (t.ctx.native_mode) {
    23136:            frame.native_mc = std::make_unique<darwin_x86_mcontext>();
    23137:            auto &mc = *frame.native_mc;
    23138:            mc.es = {static_cast<std::uint16_t>(t.ctx.native_trapno), 0, t.ctx.native_error, fault_addr};
    23139:            for (int i = 0; i < 16; ++i) mc.ss.registers[i] = t.ctx.native_x[darwin_x86_registers[i]];
    23141:            mc.ss.rflags = t.ctx.native_flags;
    23143:            std::memcpy(mc.fs.fx, t.ctx.native_fp, sizeof mc.fs.fx);
    23246:    selection callback
    23247:    selection callback
    23248:    selection callback
    23249:    selection callback
    23250:    selection callback
    23251:    selection callback
    23252:    selection callback
    23253:    selection callback
    23254:    selection callback
    23255:    selection callback
    23256:    selection callback
    23257:    selection callback
    23317:            std::memcpy(resumed.native_fp, mc.fs.fx, sizeof resumed.native_fp);
    23318:            std::memcpy(resumed.native_v, mc.fs.fx + 160, sizeof resumed.native_v);
    23344:             * native threads. Give each a persistent guest identity and stack
    23346:             * callback reuses g_current; a later work item on this native worker
    23347:             * reuses this binding, preserving thread identity and native TLS. */
    23369:            const bool native = p->main_->img.header->cputype == MM_CPU_TYPE_X86_64;
    23372:            std::memcpy(reinterpret_cast<void *>(t->ctx.x[31] + (native ? 16 : 0)),
    23390:        const auto native_rdx = t->ctx.native_x[MM_RDX];
    23394:        if (t->ctx.native_mode) t->ctx.native_x[MM_RDX] = native_rdx;
    23491:    selection callback
    23492:    selection callback
    23493:    selection callback
    23494:    selection callback
    23495:    selection callback
    23496:    selection callback
    23497:    selection callback
    23498:    selection callback
    23499:    selection callback
    23500:    selection callback
    23501:    selection callback
    23502:    selection callback
    23649:    $ rg -n 'dlclose|dlopen|native_libs|host_libs' src/runtime/loader.cpp src/dylibs/libSystem/*.cpp | head -n 50
    24147:## Assistant
    24149:Исправления очистки буфера и `NSPointInRect` готовы; проверка координат проходит на обеих архитектурах. Второй краш произошёл уже после создания WebView, при вызове `NSLog` с неверно прочитанными аргументами — это тоже исправлено. Сейчас разбираю падение Intel-теста при завершении, прежде чем снова запускать Roblox.
    24160:        /* Intel passes NSPoint in xmm0/xmm1 but the 32-byte NSRect on the
    24162:        if (auto *ctx = api()->current_context(); ctx && ctx->native_mode) {
    24164:            std::memcpy(rect, reinterpret_cast<const void *>(ctx->native_x[MM_RSP] + 8), sizeof rect);
    24287:        rg -n 'native_x.*MM_RDI|superclass|objc_msgSendSuper2' src/dylibs/libobjc/objc.cpp | tail -n 30
    24345:        else if (ctx->native_mode && (stret || (m->types && std::strchr(m->types, '{')))) {
    24348:                std::fprintf(stderr, "objc: cannot bridge Intel aggregate method %s with encoding %s\n", sel, m->types ? m->types : "(missing)");
    24351:            return native_abi::call_host(*ctx, m->imp, sig, reinterpret_cast<std::uintptr_t>(receiver),
    24565:    $ env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
    24566:    /usr/bin/bash: line 1:     3 Segmentation fault         (core dumped) env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
    24571:    $ tail -n 55 work/logs/text-view-native-trace.log
    24652:    selection callback
    24653:    selection callback
    24654:    selection callback
    24655:    selection callback
    24656:    selection callback
    24657:    selection callback
    24658:    selection callback
    24659:    selection callback
    24660:    selection callback
    24661:    selection callback
    24662:    selection callback
    24663:    selection callback
    24707:        rg -n 'native_x\[MM_RSP\]|native_entry|native_mode' src/runtime/loader.cpp | head -n 35
    24708:        sed -n '155,195p' src/dylibs/libobjc/native_abi.h
    24755:    44:extern "C" { thread_local std::uint64_t mm_native_entry_pc = 0; }
    24756:    49:              offsetof(mm_cpu_context, native_mode) == 1048 && offsetof(mm_cpu_context, native_v) == 1056,
    24757:    1588:        t.ctx.native_mode = 1;
    24758:    1592:        t.ctx.native_x[MM_RSP] = t.ctx.x[31] - 8;
    24759:    1594:        std::memcpy(reinterpret_cast<void *>(t.ctx.native_x[MM_RSP]), initial_stack, sizeof initial_stack);
    24760:    1627:                t.ctx.x[31] = t.ctx.native_x[MM_RSP];
    24761:    1633:            const auto sp = t.ctx.native_x[MM_RSP];
    24762:    1796:                t.ctx.native_x[MM_RSP] += 8;
    24763:    2044:    if (t.ctx.native_mode) {
    24764:    2080:    if (t.ctx.native_mode) {
    24765:    2081:        const auto sp = ((t.ctx.native_x[MM_RSP] - 1024) & ~std::uint64_t{15}) - 8;
    24766:    2088:        t.ctx.native_x[MM_RSP] = sp;
    24767:    2187:    if (t->ctx.native_mode) t->ctx.native_x[MM_RDX] = native_rdx;
    24783:            if (!result_buffer) throw std::runtime_error("Intel aggregate return requires objc_msgSend_stret");
    24804:                if (!result_buffer) throw std::runtime_error("Intel aggregate return requires objc_msgSend_stret");
    24818:    +    /* An Intel aggregate adapter temporarily uses x[31] for flattened
    24820:    +     * callbacks must keep using the real Intel stack, below its red zone. */
    24821:    +    const auto callback_sp = saved.native_mode ? saved.native_x[MM_RSP] : saved.x[31];
    ✓ • 0ms

## Activity

    $ cat AGENTS.md && sed -n '23500,24150p' codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md
    # CLAUDE.md
    
    This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.
    
    ## What this is
    
    A wine-like compatibility layer for running macOS arm64 binaries on x86_64 Windows/Linux hosts. C++26 + CMake, no external dependencies. Mach-O parsing (incl. fat binaries), the venv layout, an AOT translator covering integer A64 + AdvSIMD integer and FP (99.8% of Ableton Live's `__text`; `machoman translate --stats <bin>` shows the rest), and a runtime that links guest images against both host shared objects (libSystem, libobjc) and real arm64 Mach-O dylibs (LLVM libc++/libc++abi/libunwind cross-built as guest code, an app bundle's own dylibs). `rg` and C++ programs with exceptions run end to end; Ableton Live runs its static initializers, `main`, NSApplication startup, its embedded Python and licensing, loads `MainMenu.nib` (its application delegate comes from there), talks to its extension host over a Unix socket, builds its view tree, creates its NSWindow and is inside `InitApplication`.
    
    ## Build / test
    
    ```sh
    scripts/build-guest-runtimes.sh                    # once: guest libc++/libc++abi/libunwind; needs third_party/sdk/MacOSX*.sdk (tar -xf the SDK there)
    cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++   # clang++ is the preferred compiler; -Wall -Wextra -Wconversion
    cmake --build build
    ctest --test-dir build --output-on-failure
    ctest --test-dir build -R translator   # single test: names are `macho_parse` and `translator`
    ```
    
    Sanitizer build: add `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`. Tests are plain C++ executables under `tests/` (no framework, a local `check()` helper), registered in `tests/CMakeLists.txt`. `macho_parse` takes the sample binary path as argv[1]; `test_translator` executes translated snippets on the real CPU via `mm_runtime_exec`.
    
    To get an A64 encoding for a new test case: `clang --target=aarch64 -c x.s` and read the `.text` words (no `llvm-objdump` on this machine). To build an arm64 macOS test program: `clang++ --target=arm64-apple-macos12 -isysroot third_party/sdk/MacOSX26.1.sdk -fuse-ld=lld`, copy it into a venv's `bin/` and `machoman run`.
    
    ## Architecture
    
    Three layers, each a directory in `src/` with a matching public header in `include/machoman/`, all compiled into one static library `machoman_core` and driven by the CLI in `src/cli/main.cpp`:
    
    1. **venv** (`src/venv/`) — a directory tree standing in for a macOS root (like a WINEPREFIX) plus a JSON manifest. Dylib stubs are keyed by install-name basename.
    2. **translator** (`src/translator/`) — static AOT (not JIT) arm64 -> x86_64. Pipeline is `arm64_decoder.cpp` (A64 bits -> `mm_a64_insn` IR) -> `translator.cpp` (IR -> host sequences) -> `x86_64_encoder.cpp` (byte emission into `mm_x64_buf`).
    3. **runtime** (`src/runtime/`) — `loader.cpp`: `code_map` (process-wide two-level table guest VA -> host code, walked by the dispatch stub), `mm_runtime_code` (one per image: exec memory, registers its instructions in the code map), `linker` (install name -> host `.so` via `dlopen`/`dlsym`, or guest Mach-O dylib: mapped at a free base above 0x200000000, rebased, exports indexed from the trie, dependencies loaded recursively), `guest_process` (binding with trap pages for unresolved imports, the run loop that services host calls — imports and, via `dladdr`, any host symbol such as a vtable slot —, static initializers dependency-first, guest threads on host threads, signals); `syscall_table.cpp` is unused so far (Darwin binaries call libSystem, not `svc`, and libSystem is host code).
    4. **host dylibs** (`src/dylibs/<name>/`) — real shared objects the runtime `dlopen`s. `libSystem.B.dylib.so` exports Darwin-ABI functions under their Darwin names via `__asm__("_name")` labels (`DARWIN(name)` in `internal.h`); anything it does not define resolves through its libc dependency. Variadic Darwin functions read their `...` from the guest stack (`mm_guest_va_list`), never from C varargs. Never add a symbol table to the core — new dylibs are new `.so` targets under the venv layout.
    5. **host frameworks** (`src/dylibs/Foundation/`, `src/dylibs/AppKit/`, `src/dylibs/symbol-synthesis/`) — CoreFoundation and Foundation are one object, because toll-free bridging means the types are literally the same: every CF type is an objc object of a host class (`foundation.h` has the class/method helpers; payloads are C++ structs whose first member is the objc header, constructed by `+alloc` and destroyed by `-dealloc`). Anything not implemented yet gets the stub framework, symlinked under each framework path: it defines no symbols and answers `machoman_resolve_symbol(name, flags)` with a class, a constant string or a reporting function stub. Classification is by bind kind (`MM_SYMBOL_LAZY` = call site = function), never by name — Apple's constants are full of verbs. AppKit follows the same pattern one level up (`include/machoman/objc_framework.h` holds the shared class/method helpers) and keeps everything platform-specific behind the `platform` interface in `src/dylibs/AppKit/appkit.h`, so the object model builds and runs without a display.
    6. **nib loading** (`src/dylibs/AppKit/nib.cpp`) — a `.nib` is an `NSKeyedArchiver` binary plist, so the plist parser already reads it; UIDs are a plist type of their own (`__NSCFKeyedArchiverUID`, `uid_create` in `plists.cpp`), never numbers. The loader rebuilds objects, menus, windows and connections: an outlet goes through `set<Label>:` or straight into the ivar (`class_getInstanceVariable`/`object_setIvar`), an action sets target/action. `$top`'s root key is `IB.objectdata` for Interface Builder.
    7. **guest runtimes** (`scripts/build-guest-runtimes.sh`, `third_party/guest-runtimes/`) — libc++/libc++abi/libunwind cross-compiled to arm64 Mach-O with the macOS SDK and installed into `build/dylibs/usr/lib/`. They are guest code on purpose: C++ ABI details (constructors returning `this`, sret, 16-byte returns, vtables in both directions, exception unwinding) stay inside the translated world, and only libSystem's C ABI crosses to the host. Do not reintroduce a host libc++: mixing the two C++ ABIs at the call boundary was a dead end. `scripts/patches/` holds the one libunwind patch (lld emits duplicate compact-unwind entries).
    
    CommonCrypto (`src/dylibs/libSystem/commoncrypto.cpp`) is part of libSystem on Darwin: AES and SHA-256 implemented in-tree (no external crypto), with the published test vectors in `tests/test_commoncrypto.cpp`. A crypto stub that returns success is worse than none — the caller then works on garbage.
    
    Cocoa notifications go through the one real `NSNotificationCenter` in `Foundation/misc.cpp` — an app that is not the NSApplication delegate observes `NSApplicationDidFinishLaunchingNotification` there, so `-finishLaunching` posts them as well as messaging the delegate.
    
    `src/common/macho.cpp` is the shared Mach-O 64 parser. All pointers in `mm_macho_image` alias the caller's buffer; for fat files `img.data` is the arm64 slice, so always go through `img.section_bytes()` rather than indexing the original file.
    
    ### Translator execution model (read `translator.h` first)
    
    - Guest registers live in memory: `mm_cpu_context.x[32]`, `x[31]` = SP. Host `r15` holds the context pointer; `rax`/`rcx`/`rdx` are scratch; nothing else is touched. Every guest instruction is load-operands / compute / store-result. This is deliberately simple rather than fast — do not reintroduce a fixed register map, that was the source of aliasing bugs.
    - Register 31 is resolved per opcode via `reg31::sp` / `reg31::xzr` in `translator.cpp`; `add/sub imm` and `ldr/str` bases are SP, everything else is XZR, and `rd` of a flag-setting op (`cmp`, `tst`) is XZR.
    - 32-bit (`w`) forms compute in 64 bits and zero-extend on store; the invariant is that a `w` register's upper half is always zero in the context.
    - Anything the decoder does not model exactly must decode as `unknown`. A wrong translation marked supported is worse than a gap. Unsupported instructions emit `ud2` and bump `unsupported_count`, the coverage metric (`machoman translate third_party/samples/rg-arm64`).
    - NZCV: saved to `ctx.eflags` with `pushf` right after each flag-setting x86 op (`cmc` after add, `stc` after logic so that `CF == NOT C` always), restored with `popf` before each consumer. `host_cc()` maps every A64 condition to one jcc under that convention. Never read EFLAGS across guest instructions — scratch arithmetic clobbers them.
    - Control flow: each image's code is translated in one pass, so direct branches are rel32 fixups to `notes[idx].output_offset`; indirect ones go through the shared dispatch stub, which walks the process-wide code map (`ctx.branch_table` = L1, `MM_CODE_MAP_*` in translator.h: 16 KiB guest pages, one host address per instruction). Everything that cannot continue (host address, trap page, untranslated VA) jumps to an exit stub that sets `ctx.exit_reason`/`fault_va` and `ret`s to the trampoline; the top-level `ret` returns via the `x30 == 0` sentinel.
    - PC-relative forms (adr/adrp, ldr literal) are resolved at translate time against `base_va` (the section's `addr`); `mm_runtime_launch` maps segments at their vmaddr with `MAP_FIXED_NOREPLACE` so those addresses are real.
    - Guest memory faults are caught by a SIGSEGV handler that `siglongjmp`s out of the translated code and maps host rip back to the guest PC (`guest_pc_for`). Translated code holds no host state, so this is safe.
    - x86 traps that AArch64 does not have (`div` by zero, `INT_MIN/-1`) are branched around in `emit_div`; keep that pattern for any future op with host-only faults.
    - Test snippets that end in `ret` must leave `x30 == 0` (the harness relies on the sentinel); a `bl` inside a snippet has to restore it, or the top-level `ret` loops forever.
    - SIMD emission (`translator_simd.cpp`): xmm0-xmm3 are scratch (`X0..X3` in `emit.h`); every op is `vload` -> SSE -> `vstore(q)`; 128-bit constants come from `vconst` (GPR + `movq`/`pinsrq`), there is no constant pool. Q=0 forms must zero the high half (`vstore(..., false)` does).
    - Decoding NEON: bit 28 set + bit 30 set = scalar SIMD; bit 28 set + bit 30 clear = scalar FP — never let FP encodings fall into the integer paths. When unsure what a word is, `python3 -c 'from capstone import *; ...'` (capstone is installed) beats guessing.
    - An unrecognized selector is fatal only on a guest class. On a class machoman implements (or a guest subclass of one), it prints the selector and answers nil, because these classes are deliberately partial — Apple's have hundreds of methods — and stopping at the first gap hides every gap behind it. `MACHOMAN_STRICT_SELECTORS=1` makes both fatal.
    - Darwin's constants are not Linux's, and the differences are silent: `SIG_SETMASK` is 3 there and 2 here, `SOL_SOCKET` is 0xFFFF against 1, every `SO_*` differs, and a `sockaddr` begins with its own length byte rather than a 16-bit family. `socket.cpp` and the signal-mask helpers in `libSystem.cpp` translate them; anything new that takes a system constant needs the same treatment.
    - A host call gets x0-x7 plus six words from the guest stack (`hostcall_x86_64.S`), because AArch64 passes everything past x7 there. A host function of more than eight integer arguments — `CFStringGetBytes` writes its byte count through the ninth — read a stack slot nobody filled before that, and returned garbage instead of failing.
    - Host method IMPs get the guest's registers verbatim: integer arguments in order, doubles in order, structs flattened (an NSRect method takes four doubles). Returns go back the same way — `mm_objc_return_doubles_hi` for the upper half of an NSRect/CGPoint, `mm_objc_return_words_hi` for the second word of an NSRange. The hostcall trampoline deliberately does not copy rdx into x1; that broke guest code that still needed x1 after a call.
    - Exported *class* storage (`MM_OBJC_CLASS_STORAGE`, `FND_CLASS`, `MM_FW_CLASS`) must sit at namespace scope: inside an anonymous namespace it gets internal linkage and the guest's bind silently traps instead.
    - A host class's declared instance size must match the payload struct that `+alloc` placement-news into it; declaring a class with `sizeof(mm_objc_object)` and then constructing a bigger payload corrupts the heap.
    - Host functions that take a callback (qsort, pthread_once, a CF apply function) must route it through `mm_call_callback`/`call_guest`: the pointer is guest code and calling it directly jumps into translated bytes.
    - The Process Manager (`GetNextProcess`/`GetProcessInformation` in `carbon.cpp`) enumerates exactly one application — this one, with the bundle's own `CFBundleSignature`. Reporting *no* processes is not neutral: a program counting the copies of itself finds zero and concludes it is the second instance. Ableton Live did exactly that, and as the second instance it opened no database and never showed a window.
    - AppKit sends the launch-time delegate messages itself: `-applicationDidFinishLaunching:` and, for an app launched without a document, `-applicationShouldOpenUntitledFile:`/`-applicationOpenUntitledFile:` (the open-application Apple Event). `MACHOMAN_APPKIT_TRACE=1` prints which of those the delegate actually implements, and `MACHOMAN_APPKIT_FORCE_SHOW=1` orders every window on screen after launch — the way to tell "nothing draws the window" from "the program never asked for it".
    - `MACHOMAN_OBJC_BACKTRACE=frame,screen` prints the calling guest's stack every time one of those selectors is sent. The objc trace says which message was sent; only this says which of the guest's own functions sent it, which is the question whenever a program stops doing anything.
    - `MACHOMAN_APPKIT_TRACE=1` traces the window system alone (windows, nib loading, the Wayland connection); `MACHOMAN_TRACE=1` also traces every host call, which is thousands of lines a second. A Live run takes ~100 s to reach its interface, so run it with a `timeout` in the background and grep the log.
    - Debugging a guest's own diagnostics: `MACHOMAN_OSLOG=1` decodes the argument buffer of `_os_log_impl`, which is how an app's internal errors become readable; `api->print_guest_backtrace()` prints the calling guest thread's stack from inside a host function.
    - dyld's environment variables are honoured in `locate()`: `DYLD_FRAMEWORK_PATH`/`DYLD_LIBRARY_PATH` are tried before the install name (they override an absolute path inside the binary — that is what they are for), `DYLD_FALLBACK_*` after it, `DYLD_ROOT_PATH` is a root ahead of the venv, `DYLD_INSERT_LIBRARIES` becomes an extra dependency of the executable (so its initializers run and its exports are visible) and `DYLD_PRINT_LIBRARIES` turns on the `dyld:` lines. A search-path directory is a guest path, so it is tried both literally and under the venv roots. `machoman run -e KEY=VALUE` sets a variable for one run; `mm_launch_request::env` applies before anything reads the environment, and the guest inherits this process's environ.
    - Dylib name convention: lookup order is Darwin name verbatim (`_stat`) then plain (`stat`). Define a Darwin-name symbol in a host dylib whenever the host libc's semantics/ABI differ; otherwise define nothing. Host functions must return `int`/pointers, never `bool`/`char` (the guest reads `w0`, x86-64 only defines `al`). A symbol's `$` suffix is a variant marker (`_realpath$DARWIN_EXTSN`) only when what follows is not `_`: `_OBJC_CLASS_$_NSView` must be looked up whole. Exported *variables* (`___stdinp`, `__DefaultRuneLocale`) go at namespace scope with `__asm__` labels — an anonymous namespace makes them internal.
    - `MACHOMAN_WATCH=0x1002cb6dc,...` puts a watch point in front of those guest instructions: `mm_watch_hit` (translator.cpp) prints x0-x8, sp and x30 as execution passes, and any register that points at a UTF-8 or UTF-16 string prints the text too (a few header offsets are tried, since a register usually points at the object that owns the characters). It is the only way to see what a guest function returned — translated code holds no host state, so a host debugger sees the instruction but never the guest's x0.
    - Debugging a guest: `machoman disasm <bin> <va> [n]` annotates `bl` targets with symbols/imports; a fault prints a frame-pointer backtrace across images; `gdb --args machoman run ...` with `handle SIGSEGV SIGILL SIGBUS nostop noprint pass` and a breakpoint on a host function conditioned on `$_caller_is("mm_hostcall", 1)` catches guest-originated host calls.
    - Adding an instruction: enum + decode in `arm64_decoder.{h,cpp}`, a primitive emitter in `x86_64_encoder.{h,cpp}` if missing, the case in `translate_one`, and an execute test in `tests/test_translator.cpp`.
    
    ## Test fixture
    
    `third_party/samples/rg-arm64` is a real arm64 ripgrep Mach-O and the reference binary for both the parser test and translator coverage measurements.
        selection callback
        selection callback
        selection callback
        window destructor
        run: entry returned x0=0
        host signal stop
        #0  0x00007ffff7fc8f35 in ?? () from /lib64/ld-linux-x86-64.so.2
        #1  0x00007ffff7fc9b16 in ?? () from /lib64/ld-linux-x86-64.so.2
        #2  0x00007ffff7972376 in ?? () from /usr/lib/libc.so.6
        #3  0x00007ffff78945d0 in ?? () from /usr/lib/libc.so.6
        #4  0x00007ffff7fc0479 in _dl_catch_exception () from /lib64/ld-linux-x86-64.so.2
        #5  0x00007ffff7fc05c9 in ?? () from /lib64/ld-linux-x86-64.so.2
        #6  0x00007ffff7893fa3 in ?? () from /usr/lib/libc.so.6
        #7  0x00007ffff789466e in dlsym () from /usr/lib/libc.so.6
        #8  0x00005555555ff91f in (anonymous namespace)::guest_process::~guest_process (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1254
        #9  0x00005555555fbeca in mm_runtime_launch (request=...) at /mnt/media/machoman/src/runtime/loader.cpp:2644
        #10 0x0000555555564a72 in (anonymous namespace)::cmd_run (venv_dir=..., binary=..., args=..., env=..., apply_defaults=..., signal_mask=...) at /mnt/media/machoman/src/cli/main.cpp:296
        #11 0x00005555555612e5 in main (argc=..., argv=...) at /mnt/media/machoman/src/cli/main.cpp:451
        #8  0x00005555555ff91f in (anonymous namespace)::guest_process::~guest_process (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1254
        1254                    auto stop = reinterpret_cast<void (*)()>(dlsym(library->handle, name));
        $1 = "libncursesw.so.6"
        $2 = (void *) 0x555555726700
        A debugging session is active.
    
            Inferior 1 [process 215414] will be killed.
    
        Quit anyway? (y or n) [answered Y; input not from terminal]
        ✓ • 713ms
    
    ## Activity
    
        $ command -v valgrind
            sed -n '125,154p' src/dylibs/AppKit/views.cpp
            sed -n '1450,1530p' src/dylibs/Foundation/misc.cpp
                        auto *cv = v(child); auto f = cv->frame;
                        if (!cv->translates_mask) continue;
                        layout::resize_axis(f.x, f.w, previous.w, width-previous.w, cv->autoresizing_mask & 7);
                        layout::resize_axis(f.y, f.h, previous.h, height-previous.h, (cv->autoresizing_mask >> 3) & 7);
                        auto aligned = layout::align_edges({f.x, f.y, f.w, f.h});
                        f = {aligned.x, aligned.y, aligned.w, aligned.h};
                        view_setFrame(child, nullptr, f.x, f.y, f.w, f.h);
                    }
                }
                o->bounds.w = width;
                o->bounds.h = height;
                if (o->layer) {
                    if (is_kind(o->layer, cls("CALayer"))) {
                        // AppKit owns a backing layer's frame in the view's local
                        // coordinates. Presentation uses frame, not just bounds.
                        auto *layer = as<layer_object>(o->layer);
                        layer->frame = {0, 0, width, height};
                        layer->position = {width / 2, height / 2};
                    }
                    if (mm_cpu_context *ctx = api()->current_context()) {
                        const double values[4] = {0, 0, width, height};
                        for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
                    }
                    send(o->layer, "setBounds:");
                    if (mm_cpu_context *ctx = api()->current_context()) {
                        const double values[4] = {0, 0, width, height};
                        for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
                    }
                    send(o->layer, "setNeedsDisplayInRect:");
                }
    
        /* A notification is its name, the object that posted it and a user info
         * dictionary; an observer is a receiver plus the selector to send it,
         * optionally narrowed to one name and one posting object. Blocks registered
         * with a nil queue run on the posting thread; an operation queue keeps
         * callback execution on the requested queue. */
        struct notification_object {
            mm_objc_object base;
            id name = nullptr, object = nullptr, user_info = nullptr;
            ~notification_object() { release(name); release(object); release(user_info); }
        };
    
        struct observer_entry {
            id observer = nullptr;   /* not retained, as on macOS */
            SEL selector = nullptr;
            id block = nullptr;      /* retained when set; mutually exclusive with selector */
            std::string name;        /* empty = every name */
            bool any_name = true;
            id object = nullptr;     /* null = every poster */
            id token = nullptr;      /* the object -addObserverForName: returned */
            id queue = nullptr;
        };
    
        struct center_object {
            mm_objc_object base;
            std::vector<observer_entry> observers;
            std::recursive_mutex mu;
        };
    
        id notification_create(id name, id object, id user_info) {
            notification_object *n = make<notification_object>(g_notification);
            n->name = retain(name);
            n->object = retain(object);
            n->user_info = retain(user_info);
            return &n->base;
        }
    
        std::uint64_t notification_name(id self, SEL) { return ret(as<notification_object>(self)->name); }
        std::uint64_t notification_object_(id self, SEL) { return ret(as<notification_object>(self)->object); }
        std::uint64_t notification_userInfo(id self, SEL) { return ret(as<notification_object>(self)->user_info); }
        std::uint64_t notificationc_notificationWithName(id, SEL, id name, id object) {
            return ret(autorelease(notification_create(name, object, nullptr)));
        }
        std::uint64_t notificationc_notificationWithNameUserInfo(id, SEL, id name, id object, id info) {
            return ret(autorelease(notification_create(name, object, info)));
        }
    
        center_object *center_of(id self) { return as<center_object>(self); }
    
        void center_add(id self, id observer, SEL sel, id block, id name, id object, id token, id queue = nullptr) {
            center_object *c = center_of(self);
            observer_entry e;
            e.observer = observer;
            e.selector = sel;
            e.block = block ? retain(block) : nullptr;
            e.any_name = name == nullptr;
            if (name) e.name = to_utf8(name);
            e.object = object;
            e.token = token;
            e.queue = retain(queue);
            std::lock_guard lock(c->mu);
            c->observers.push_back(e);
        }
    
        std::uint64_t center_addObserver(id self, SEL, id observer, SEL sel, id name, id object) {
            center_add(self, observer, sel, nullptr, name, object, nullptr);
            return 0;
        }
        std::uint64_t center_addObserverForName(id self, SEL, id name, id object, id queue, id block) {
            /* The token is what the caller later passes to -removeObserver:. */
            id token = mm_objc_alloc(cls("NSObject"), 0);
            center_add(self, nullptr, nullptr, block, name, object, token, queue);
            return ret(token);
        }
        std::uint64_t center_removeObserver(id self, SEL, id observer) {
            center_object *c = center_of(self);
            std::lock_guard lock(c->mu);
            std::erase_if(c->observers, [&](const observer_entry &e) {
                const bool gone = e.observer == observer || (e.token && e.token == observer);
                if (gone && e.block) release(e.block);
                if (gone) release(e.queue);
        ✓ • 0ms
    
    ## Activity
    
        $ rg -n 'dlclose|dlopen|native_libs|host_libs' src/runtime/loader.cpp src/dylibs/libSystem/*.cpp | head -n 50
            sed -n '1,140p' tests/guest_text_view.m | tail -n 35
            rg -n 'objc_autoreleasePoolPop|t_pools|do_release' src/dylibs/libobjc/objc.cpp | tail -n 20
        src/runtime/loader.cpp:628:        for (auto &l : host_libs_)
        src/runtime/loader.cpp:629:            if (l->handle) dlclose(l->handle);
        src/runtime/loader.cpp:634:    std::vector<std::unique_ptr<host_lib>> &host_libs() { return host_libs_; }
        src/runtime/loader.cpp:642:        for (std::size_t index = 0; index < host_libs_.size(); ++index) {
        src/runtime/loader.cpp:643:            if (!host_libs_[index]->handle) continue;
        src/runtime/loader.cpp:645:            if (dlinfo(host_libs_[index]->handle, RTLD_DI_LINKMAP, &map) != 0 || !map || !map->l_ld) continue;
        src/runtime/loader.cpp:653:                void *handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
        src/runtime/loader.cpp:655:                const bool known = std::any_of(host_libs_.begin(), host_libs_.end(),
        src/runtime/loader.cpp:657:                if (known) { dlclose(handle); continue; }
        src/runtime/loader.cpp:660:                    dlclose(handle); continue;
        src/runtime/loader.cpp:666:                host_libs_.push_back(std::move(library));
        src/runtime/loader.cpp:715:            for (auto &h : host_libs_)
        src/runtime/loader.cpp:726:            for (auto &h : host_libs_)
        src/runtime/loader.cpp:735:            lib->handle = dlopen(host_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        src/runtime/loader.cpp:741:            host_libs_.push_back(std::move(lib));
        src/runtime/loader.cpp:742:            return {nullptr, host_libs_.back().get()};
        src/runtime/loader.cpp:803:        for (auto &h : host_libs_)
        src/runtime/loader.cpp:822:        for (auto &h : host_libs_) {
        src/runtime/loader.cpp:1066:    std::vector<std::unique_ptr<host_lib>> host_libs_;
        src/runtime/loader.cpp:1253:            for (auto &library : linker_.host_libs()) if (library->handle) {
        src/runtime/loader.cpp:1513:    for (auto &h : linker_.host_libs()) {
        src/runtime/loader.cpp:1520:    for (auto &h : linker_.host_libs()) {
        src/runtime/loader.cpp:1910:    for (auto &h : linker_.host_libs()) {
        src/runtime/loader.cpp:2550:    report.host_libraries = ld.host_libs().size();
        src/dylibs/libSystem/libSystem.cpp:938:    static void *libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        src/dylibs/libSystem/libSystem.cpp:957:    static void *libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
                check(editor.string.length == 0 && editor.selectedRange.location == 0, "delete UTF-16 surrogate pair");
                editor.string = @"ab";
                editor.selectedRange = NSMakeRange(0, 1);
                [editor setMarkedText:@"x" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
                [editor setMarkedText:@"yz" selectedRange:NSMakeRange(2, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
                check([editor.string isEqualToString:@"yzb"] && editor.markedRange.length == 2, "replace existing composition");
                [editor unmarkText];
                NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 300, 200)
                    styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
                PointerProbe *render = [[PointerProbe alloc] initWithFrame:root.bounds];
                NSTextView *zeroEditor = [[NSTextView alloc] initWithFrame:NSZeroRect];
                [root addSubview:render]; [render addSubview:zeroEditor]; window.contentView = root;
                [window makeFirstResponder:zeroEditor];
                check([root hitTest:NSMakePoint(90, 70)] == render,
                    "zero-sized field editor does not intercept pointer hit testing");
                event->probeType = NSEventTypeMouseMoved; [window sendEvent:event];
                event->probeType = NSEventTypeLeftMouseDown; [window sendEvent:event];
                event->probeType = NSEventTypeLeftMouseUp; [window sendEvent:event];
                check(render->moves == 1 && render->downs == 1 && render->ups == 1,
                    "window sends pointer events to rendering view while field editor has keyboard focus");
                NSStackView *container = [NSStackView stackViewWithViews:@[[NSView new]]];
                NSBox *box = [NSBox new];
                [box addSubview:container]; [root addSubview:box];
                check([container isKindOfClass:NSView.class] && container.window == window &&
                    container.arrangedSubviews.count == 1 && ((NSView *)container.arrangedSubviews[0]).window == window,
                    "stack containers have a real view payload and recursively inherit window membership");
                [window makeFirstResponder:nil]; [zeroEditor release]; [render release]; [root release];
                [box release]; [window release];
                editor.delegate = nil;
                [delegate release]; [event release]; [editor release];
            }
            puts("NSTextView field editor, guest text callbacks and UTF-16 range ABI passed");
            return 0;
        }
        404:thread_local std::vector<std::vector<id>> t_pools;
        407:void do_release(id obj);
        643:void do_release(id obj) {
        665:        for (auto &[key, value] : values) do_release(value.object);
        676:            if (v.policy & 1) do_release(v.value); /* retained policies have bit 0 set (1, 3) */
        693:std::uint64_t NSObject_release(id self, SEL) { do_release(self); return 0; }
        695:    if (t_pools.empty()) t_pools.emplace_back();
        696:    t_pools.back().push_back(self);
        1236:    t_pools.emplace_back();
        1237:    return reinterpret_cast<void *>(t_pools.size());
        1239:void objc_autoreleasePoolPop(void *token) DARWIN(objc_autoreleasePoolPop);
        1240:void objc_autoreleasePoolPop(void *token) {
        1242:    while (t_pools.size() >= depth && !t_pools.empty()) {
        1243:        std::vector<id> pool = std::move(t_pools.back());
        1244:        t_pools.pop_back();
        1561:void mm_objc_release(mm_id obj) { do_release(obj); }
        ✓ • 826ms
    
    ## Activity
    
        $ sed -n '638,670p' src/runtime/loader.cpp
            sed -n '638,694p' src/dylibs/libobjc/objc.cpp
            sed -n '1236,1252p' src/dylibs/libobjc/objc.cpp
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
            std::uint64_t none = 0;
            (void)none;
            mm_objc_send(obj, sel_intern("dealloc"), 0, nullptr);
        }
    
        void do_release(id obj) {
            if (!obj) return;
            if (is_block_object(obj)) { block_release(obj); return; }
            bool dealloc = false;
            {
                std::lock_guard lock(g_lock);
                auto it = g_refcounts.find(obj);
                if (it == g_refcounts.end()) return; /* never retained by us: static object */
                if (--it->second.count == 0) {
                    if (!it->second.owned) { g_refcounts.erase(it); return; }
                    if (g_deallocating.insert(obj).second) dealloc = true;
                }
            }
            if (dealloc) do_dealloc(obj);
        }
    
        void free_instance(id obj) {
            mm_objc_destruct_instance(obj);
            std::lock_guard lock(g_lock);
            if (auto it = T().stub_values.find(obj); it != T().stub_values.end()) {
                auto values = std::move(it->second);
                T().stub_values.erase(it);
                for (auto &[key, value] : values) do_release(value.object);
            }
            /* zero weak references */
            if (auto w = g_weak.find(obj); w != g_weak.end()) {
                for (id *slot : w->second) *slot = nullptr;
                g_weak.erase(w);
            }
            if (auto a = g_assoc.find(obj); a != g_assoc.end()) {
                auto copies = std::move(a->second);
                g_assoc.erase(a);
                for (auto &[k, v] : copies)
                    if (v.policy & 1) do_release(v.value); /* retained policies have bit 0 set (1, 3) */
            }
            if (auto s = g_sync.find(obj); s != g_sync.end()) { delete s->second; g_sync.erase(s); }
            g_refcounts.erase(obj);
            g_deallocating.erase(obj);
            std::free(obj);
        }
    
        /* NSObject methods (host IMPs: (self, _cmd, ...)) */
        std::uint64_t NSObject_alloc(id cls, SEL) { return reinterpret_cast<std::uintptr_t>(mm_objc_alloc(reinterpret_cast<Class>(cls), 0)); }
        std::uint64_t NSObject_new(id cls, SEL) {
            id obj = reinterpret_cast<id>(mm_objc_send(cls, sel_intern("alloc"), 0, nullptr));
            return mm_objc_send(obj, sel_intern("init"), 0, nullptr);
        }
        std::uint64_t NSObject_init(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self); }
        std::uint64_t NSObject_dealloc(id self, SEL) { free_instance(self); return 0; }
        std::uint64_t NSObject_retain(id self, SEL) { return reinterpret_cast<std::uintptr_t>(do_retain(self)); }
        std::uint64_t NSObject_release(id self, SEL) { do_release(self); return 0; }
        std::uint64_t NSObject_autorelease(id self, SEL) {
            t_pools.emplace_back();
            return reinterpret_cast<void *>(t_pools.size());
        }
        void objc_autoreleasePoolPop(void *token) DARWIN(objc_autoreleasePoolPop);
        void objc_autoreleasePoolPop(void *token) {
            const auto depth = reinterpret_cast<std::uintptr_t>(token);
            while (t_pools.size() >= depth && !t_pools.empty()) {
                std::vector<id> pool = std::move(t_pools.back());
                t_pools.pop_back();
                for (auto it = pool.rbegin(); it != pool.rend(); ++it) objc_release(*it);
            }
        }
        void objc_destructInstance(id obj) DARWIN(objc_destructInstance);
        void objc_destructInstance(id obj) {
            std::lock_guard lock(g_lock);
            if (auto w = g_weak.find(obj); w != g_weak.end()) { for (id *s : w->second) *s = nullptr; g_weak.erase(w); }
            g_assoc.erase(obj);
        ✓ • 0ms
    
    ## Activity
    
        $ rg -n 'mm_runtime_status guest_process::run_main' src/runtime/loader.cpp
            sed -n '1900,1930p' src/runtime/loader.cpp
            sed -n '1,80p' src/dylibs/Foundation/dispatch.cpp
            sed -n '116,140p' CMakeLists.txt
        1883:mm_runtime_status guest_process::run_main() {
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
        /* libdispatch: queues, and the blocks a program puts on them.
         *
         * It lives beside the run loop rather than in libSystem because the main
         * queue *is* the main run loop: a block sent there has to run on the
         * thread that owns the event loop, between events, and that loop is the
         * one in Foundation/AppKit. A dispatch_async whose block never runs is
         * invisible — the work simply does not happen — and an app that schedules
         * showing its window that way (Ableton Live does) waits forever.
         *
         * Blocks are guest objects, so they are invoked through libobjc rather
         * than called as host code. Serial queues get a thread each and the
         * global queues share a small pool, which is what the API promises:
         * ordering within a serial queue, concurrency between queues. */
        #include "foundation.h"
        #include "machoman/objc_framework.h"
    
        #include <atomic>
        #include <chrono>
        #include <condition_variable>
        #include <cstdint>
        #include <cstdio>
        #include <cstdlib>
        #include <deque>
        #include <memory>
        #include <mutex>
        #include <string>
        #include <thread>
        #include <vector>
    
        namespace fnd {
    
        namespace {
    
        struct work_item {
            void *block = nullptr;                 /* retained guest block, or */
            void *function = nullptr;              /* a plain function... */
            void *context = nullptr;               /* ...with this context */
        };
    
        /* The queue traffic is part of the window-system trace: a block that is
         * queued and never run is invisible otherwise. */
        bool dispatch_trace() {
            static const bool on = std::getenv("MACHOMAN_TRACE") || std::getenv("MACHOMAN_APPKIT_TRACE");
            return on;
        }
    
        /* A function pointer from the guest has to go through the runtime. */
        void call_function(void *fn, const std::uint64_t *args, int n) {
            if (!fn || !api()) return;
            const auto va = reinterpret_cast<std::uintptr_t>(fn);
            if (api()->is_guest_code(va)) api()->call_guest(va, args, n);
            else reinterpret_cast<void (*)(std::uint64_t)>(fn)(args && n ? args[0] : 0);
        }
    
        void run_item(const work_item &item) {
            mmfw::autorelease_pool pool;
            if (item.block) {
                mm_objc_block_invoke(item.block, 0, nullptr);
                release(reinterpret_cast<id>(item.block));
            } else if (item.function) {
                const std::uint64_t arg = reinterpret_cast<std::uintptr_t>(item.context);
                call_function(item.function, &arg, 1);
            }
        }
    
        /* A queue is either the main one (drained by the run loop), a serial one
         * with a thread of its own, or a global one served by a shared pool. */
        struct dispatch_queue {
            std::string label;
            bool is_main = false;
            bool concurrent = false;
            std::mutex mu;
            std::condition_variable cv;
            std::deque<work_item> items;
            std::vector<std::thread> workers;
            std::atomic<bool> stop{false};
    
            void push(const work_item &item) {
                {
                    std::lock_guard lock(mu);
    
            # CoreFoundation + Foundation: one object, since the types are the same
            # (toll-free bridging) and both frameworks' install names point at it.
            add_library(Foundation SHARED
                src/dylibs/Foundation/strings.cpp src/dylibs/Foundation/collections.cpp src/dylibs/Foundation/index_set.cpp
                src/dylibs/Foundation/plists.cpp src/dylibs/Foundation/misc.cpp src/dylibs/Foundation/task.cpp src/dylibs/Foundation/dispatch.cpp
                src/dylibs/Foundation/dispatch_data.cpp
                src/dylibs/Foundation/cookies.cpp
                src/dylibs/Foundation/key_value.cpp src/dylibs/Foundation/threading.cpp
                src/dylibs/Foundation/geometry.cpp
                src/dylibs/Foundation/formatting.cpp
                src/dylibs/symbol-synthesis/synthesise.cpp)
            target_include_directories(Foundation PRIVATE src/dylibs/symbol-synthesis)
            target_link_libraries(Foundation PRIVATE machoman_core libobjc)
            target_compile_options(Foundation PRIVATE -Wall -Wextra -fvisibility=hidden)
            set_target_properties(Foundation PROPERTIES
                BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib" INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
                PREFIX "" OUTPUT_NAME "CoreFoundation" SUFFIX ".so"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A")
            add_custom_command(TARGET Foundation POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C"
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "../../../CoreFoundation.framework/Versions/A/CoreFoundation.so"
                    "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so")
    
        ✓ • 0ms
    
    ## Activity
    
        $ rg -n 'instanceStart|instance_size.*delta|delta|slide|ivars' src/dylibs/libobjc/objc.cpp | head -n 45
            sed -n '180,265p' src/dylibs/libobjc/objc.cpp
            rg -n 'view_object::~' src/dylibs/AppKit/*.cpp
        67:    std::uint32_t flags, instanceStart, instanceSize, reserved;
        72:    const void *ivars;
        258:    std::uint32_t start = ro->instanceStart, size = ro->instanceSize;
        262:            const std::uint32_t delta = super_size - start;
        263:            if (ro->ivars) {
        264:                const auto *il = static_cast<const ivar_list_t *>(ro->ivars);
        267:                    if (iv[i].offset) *iv[i].offset += static_cast<std::int32_t>(delta);
        269:            size += delta;
        573:            /* These methods describe only this class's ivars. An inherited
        842:void load_image(const void *header, std::uint64_t slide) {
        847:    /* Section vmaddres are unslid; the executable maps at slide 0 and the
        862:        auto *refs = reinterpret_cast<const char **>(s.selrefs->addr + slide);
        867:        auto *protos = reinterpret_cast<protocol_t **>(s.protolist->addr + slide);
        876:        auto *list = reinterpret_cast<Class *>(s.classlist->addr + slide);
        884:        auto *list = reinterpret_cast<const category_t **>(s.catlist->addr + slide);
        1115:            if (const void *header = api->image_header(i)) load_image(header, api->image_slide(i));
        1471:        if (!r->ro || !r->ro->ivars) continue;
        1472:        const auto *il = static_cast<const ivar_list_t *>(r->ro->ivars);
    
        SEL sel_intern_copy(const char *name) {
            std::lock_guard lock(g_lock);
            auto it = g_selectors.find(name);
            if (it != g_selectors.end()) return it->second;
            auto *s = new std::string(name);
            g_selector_storage.push_back(s);
            g_selectors.emplace(std::string_view(*s), s->c_str());
            return s->c_str();
        }
    
        bool is_guest(const void *p) {
            return g_api && g_api->is_guest_code && g_api->is_guest_code(reinterpret_cast<std::uintptr_t>(p));
        }
    
        /* Appends the methods of an Apple method list (big or relative). */
        void add_method_list(class_rw *r, const void *listp, bool guest_image) {
            if (!listp) return;
            const auto *list = static_cast<const method_list_t *>(listp);
            const bool relative = list->entsize_flags & 0x80000000u;
            const std::uint32_t entsize = list->entsize_flags & 0xFFFCu;
            const auto *base = reinterpret_cast<const std::uint8_t *>(list + 1);
            std::vector<method_entry> added;
            for (std::uint32_t i = 0; i < list->count; i++) {
                const std::uint8_t *e = base + i * entsize;
                method_entry m{};
                if (relative) {
                    const auto *rm = reinterpret_cast<const method_rel *>(e);
                    /* name field points at a selector reference slot (already uniqued) */
                    const auto *selref = reinterpret_cast<const SEL *>(e + rm->name);
                    m.sel = sel_intern(*selref);
                    m.types = reinterpret_cast<const char *>(e + 4 + rm->types);
                    m.imp = reinterpret_cast<void *>(const_cast<std::uint8_t *>(e) + 8 + rm->imp);
                } else {
                    const auto *bm = reinterpret_cast<const method_big *>(e);
                    m.sel = sel_intern(bm->name);
                    m.types = bm->types;
                    m.imp = bm->imp;
                }
                m.guest = guest_image || is_guest(m.imp);
                added.push_back(m);
            }
            /* Later lists take precedence: insert at the front, preserving list order within. */
            r->methods.insert(r->methods.begin(), added.begin(), added.end());
            invalidate_method_caches(r);
        }
    
        void add_protocol_list(class_rw *r, const void *listp) {
            if (!listp) return;
            const auto *list = static_cast<const protocol_list_t *>(listp);
            const auto *protos = reinterpret_cast<protocol_t *const *>(list + 1);
            for (std::uintptr_t i = 0; i < list->count; i++)
                r->protocols.push_back(protos[i]);
        }
    
        Class realize(Class c);
    
        /* Guest classes: metadata from the image. Ivars are slid when the
         * superclass turned out bigger than the class was compiled against. */
        const char *unresolved_name(const void *p) {
            return g_api && g_api->unresolved_import_name ? g_api->unresolved_import_name(reinterpret_cast<std::uintptr_t>(p)) : nullptr;
        }
    
        void realize_guest(Class c) {
            class_rw *r = rw(c);
            const class_ro_t *ro = r->ro;
            Class super = c->superclass;
            if (const char *missing = super ? unresolved_name(super) : nullptr) {
                r->missing_super = missing;
                r->realized = true;
                r->instance_size = ro->instanceSize;
                if (trace()) std::fprintf(stderr, "objc: class %s: superclass %s is not provided by any host framework\n", r->name.c_str(), missing);
                return;
            }
            if (super) {
                realize(super);
                if (rw(super)->missing_super) { r->missing_super = rw(super)->missing_super; r->realized = true; r->instance_size = ro->instanceSize; return; }
            }
            std::uint32_t start = ro->instanceStart, size = ro->instanceSize;
            if (super && !r->is_meta) {
                const std::uint32_t super_size = rw(super)->instance_size;
                if (super_size > start) {
                    const std::uint32_t delta = super_size - start;
                    if (ro->ivars) {
                        const auto *il = static_cast<const ivar_list_t *>(ro->ivars);
                        const auto *iv = reinterpret_cast<const ivar_t *>(il + 1);
        src/dylibs/AppKit/layers.cpp:41:view_object::~view_object() {
        ✓ • 0ms
    
    ## Activity
    
        file changes: Completed · 1 changes
        Add: /mnt/media/machoman/work/text-view-handle-watch.gdb
        set pagination off
        set debuginfod enabled off
        set print frame-arguments none
        set environment MACHOMAN_APPKIT_HEADLESS 1
        unset environment WAYLAND_DISPLAY
        unset environment DISPLAY
        handle SIGSEGV SIGILL SIGBUS nostop noprint pass
        break '(anonymous namespace)::guest_process::run_main'
        run
        python
        libraries = gdb.parse_and_eval('linker_.host_libs_')
        for _, value in gdb.default_visualizer(libraries).children():
            children = list(gdb.default_visualizer(value).children())
            pointer = children[0][1]
            library = pointer.dereference()
            if 'libncursesw' in str(library['install_name']):
                addr = int(library['handle'])
                gdb.execute('watch *(unsigned long*)%d' % addr)
                break
        end
        continue
        bt 25
    
    ## Activity
    
        $ gdb -q -batch -x work/text-view-handle-watch.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Breakpoint 1 at 0xaad3d: file /mnt/media/machoman/src/runtime/loader.cpp, line 1884.
        [Thread debugging using libthread_db enabled]
        Using host libthread_db library "/usr/lib/libthread_db.so.1".
    
        Breakpoint 1, (anonymous namespace)::guest_process::run_main (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1884
        1884        const mm_section_64 *text = main_->img.find_section("__TEXT", "__text");
        Hardware watchpoint 2: *(unsigned long*)93824994141952
        run: entry returned x0=0
    
        Program terminated with signal SIGSEGV, Segmentation fault.
        The program no longer exists.
        work/text-view-handle-watch.gdb:22: Error in sourced command file:
        No stack.
        ✗ (1) • 1.46s
    
    ## Assistant
    
    Исправления очистки буфера и `NSPointInRect` готовы; проверка координат проходит на обеих архитектурах. Второй краш произошёл уже после создания WebView, при вызове `NSLog` с неверно прочитанными аргументами — это тоже исправлено. Сейчас разбираю падение Intel-теста при завершении, прежде чем снова запускать Roblox.
    
    ✓ • 280ms

## Activity

    $ python3 -c 'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text(); parts=s.split("## User\n"); print("\n".join(p.split("\n## ",1)[0][:9000] for p in parts[-6:]))' && sed -n '320,390p' tests/CMakeLists.txt && sed -n '2150,2195p' src/runtime/loader.cpp && ls -ld /tmp/machoman-roblox-input-build
    
    ввод заработал, но криво, я могу писать но пока что это неюазабельно, нажатия мышкой до сих пор не работают
    
    
    ввод почему то выбрасывает то что я ввел только при нажатии на win+space т.е смену раскладки на русскую или английскую
    
    
    он еще и маппит все подряд похоже, и табы, и капс лок, и ctrl, короче служебные символы маппятся и вставляются в поле ввода
    
    
    я кое как смог ввести пароль но оно в итоге крашнулось похоже на инициализации веб view
    
    
    [Image #1] когда я ввожу пароль уже лучше, но он дублирует то что уже было написано
    
    
    а еще после ввода пароля он снова крашнулся, кстати мышь до сих пор не работала
    
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework Foundation
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_http_cookies.m" -o "${MM_COOKIE_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_http_cookies.m VERBATIM)
            add_custom_target(guest_http_cookies_${MM_COOKIE_ARCH}_binary ALL DEPENDS "${MM_COOKIE_BINARY}")
            add_test(NAME guest_http_cookies_${MM_COOKIE_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" http-cookies-${MM_COOKIE_ARCH})
            set_tests_properties(guest_http_cookies_${MM_COOKIE_ARCH} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "Cocoa HTTP cookie parsing, request headers and storage passed")
        endforeach()
        foreach(MM_URL_ARCH IN ITEMS arm64 x86_64)
            set(MM_URL_BINARY "${MM_SMOKE_VENV}/bin/file-urls-${MM_URL_ARCH}")
            add_custom_command(OUTPUT "${MM_URL_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_URL_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework Foundation
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_file_urls.m" -o "${MM_URL_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_file_urls.m VERBATIM)
            add_custom_target(guest_file_urls_${MM_URL_ARCH}_binary ALL DEPENDS "${MM_URL_BINARY}")
            add_test(NAME guest_file_urls_${MM_URL_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" file-urls-${MM_URL_ARCH})
            set_tests_properties(guest_file_urls_${MM_URL_ARCH} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "Darwin file URL constructors and base resolution passed")
        endforeach()
        foreach(MM_FORMAT_ARCH IN ITEMS arm64 x86_64)
            set(MM_FORMAT_BINARY "${MM_SMOKE_VENV}/bin/string-format-${MM_FORMAT_ARCH}")
            add_custom_command(OUTPUT "${MM_FORMAT_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_FORMAT_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework Foundation
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_string_format.m" -o "${MM_FORMAT_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_string_format.m VERBATIM)
            add_custom_target(guest_string_format_${MM_FORMAT_ARCH}_binary ALL DEPENDS "${MM_FORMAT_BINARY}")
            add_test(NAME guest_string_format_${MM_FORMAT_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" string-format-${MM_FORMAT_ARCH})
            set_tests_properties(guest_string_format_${MM_FORMAT_ARCH} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "Darwin NSString and CFString formatting passed")
        endforeach()
        foreach(MM_GEOMETRY_ARCH IN ITEMS arm64 x86_64)
            set(MM_GEOMETRY_BINARY "${MM_SMOKE_VENV}/bin/geometry-${MM_GEOMETRY_ARCH}")
            add_custom_command(OUTPUT "${MM_GEOMETRY_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_GEOMETRY_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework AppKit
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_geometry.m" -o "${MM_GEOMETRY_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_geometry.m VERBATIM)
            add_custom_target(guest_geometry_${MM_GEOMETRY_ARCH}_binary ALL DEPENDS "${MM_GEOMETRY_BINARY}")
            add_test(NAME guest_geometry_${MM_GEOMETRY_ARCH}
                COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
                    $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" geometry-${MM_GEOMETRY_ARCH})
            set_tests_properties(guest_geometry_${MM_GEOMETRY_ARCH} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "Darwin Cocoa aggregate arguments, returns and superclass dispatch passed")
        endforeach()
        set(MM_NATIVE_BINARY "${MM_SMOKE_VENV}/bin/native-smoke")
        foreach(MM_ERRNO_ARCH IN ITEMS arm64 x86_64)
            set(MM_ERRNO_BINARY "${MM_SMOKE_VENV}/bin/errno-${MM_ERRNO_ARCH}")
            add_custom_command(OUTPUT "${MM_ERRNO_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_ERRNO_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_errno.c" -o "${MM_ERRNO_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_errno.c VERBATIM)
            add_custom_target(guest_errno_${MM_ERRNO_ARCH}_binary ALL DEPENDS "${MM_ERRNO_BINARY}")
            add_test(NAME guest_errno_${MM_ERRNO_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" errno-${MM_ERRNO_ARCH})
            set_tests_properties(guest_errno_${MM_ERRNO_ARCH} PROPERTIES TIMEOUT 10
                PASS_REGULAR_EXPRESSION "Guest errno assignments, cached pointers, native libc, number conversion and thread isolation passed")
        endforeach()
        foreach(MM_PASSWD_ARCH IN ITEMS arm64 x86_64)
            set(MM_UNAME_BINARY "${MM_SMOKE_VENV}/bin/uname-${MM_PASSWD_ARCH}")
            add_custom_command(OUTPUT "${MM_UNAME_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_PASSWD_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_uname.c" -o "${MM_UNAME_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_uname.c VERBATIM)
            add_custom_target(guest_uname_${MM_PASSWD_ARCH}_binary ALL DEPENDS "${MM_UNAME_BINARY}")
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
    drwxr-xr-x 9 monsler monsler 400 Nov 13 04:55 /tmp/machoman-roblox-input-build
    ✓ • 2.46s

## Activity

    $ rg --files work | rg '(native|menu-demo|geometry|signal|closure)' && rg -n 'native|menu-demo|geometry' tests/CMakeLists.txt CMakeLists.txt
    work/roblox-native-fault.gdb
    work/guest-geometry-smoke.s
    work/changes/native-dialogs-evilcocoa.patch
    work/logs/roblox-native-13.log
    work/logs/roblox-native-12.log
    work/logs/roblox-native-11.log
    work/logs/roblox-native-10.log
    work/logs/roblox-native-10-host-stack.log
    work/logs/roblox-native-09.log
    work/logs/roblox-native-08.log
    work/logs/roblox-native-07.log
    work/logs/roblox-native-06.log
    work/logs/roblox-native-05.log
    work/logs/roblox-native-04.log
    work/logs/roblox-native-03.log
    work/logs/roblox-native-02.log
    work/logs/text-view-native-trace.log
    work/logs/setjmp-signal-stack-tests.log
    work/logs/roblox-native-build-02.log
    work/logs/roblox-native-30.log
    work/logs/roblox-native-14.log
    work/logs/roblox-native-01.log
    work/logs/native-menu-initial-opengl-demo.log
    work/logs/roblox-native-smoke-build.log
    work/logs/roblox-native-guard-gdb.log
    work/logs/roblox-native-guard-gdb-02.log
    work/logs/roblox-native-build.log
    work/logs/roblox-native-build-03.log
    work/logs/roblox-native-48-metal-data-private.log
    work/logs/roblox-native-47-ipc-context.log
    work/logs/roblox-native-46-dispatch-data.log
    work/logs/roblox-native-45-setjmp-signal-stack.log
    work/logs/roblox-native-44-setjmp-core.log
    work/logs/roblox-native-44-focus.log
    work/logs/roblox-native-44-core.log
    work/logs/roblox-native-43-memset-s.log
    work/logs/roblox-native-43-core.log
    work/logs/roblox-native-42-file-locks.log
    work/logs/roblox-native-41-file-url.log
    work/logs/roblox-native-41-core-exception.log
    work/logs/roblox-native-40-core-stack.log
    work/logs/roblox-native-40-core-context.log
    work/logs/roblox-native-40-atfork.log
    work/logs/roblox-native-39-host-info.log
    work/logs/roblox-native-38-locale-fix.log
    work/logs/roblox-native-37-inet-fix.log
    work/logs/roblox-native-37-core-stack.log
    work/logs/roblox-native-36-curl-watch.log
    work/logs/roblox-native-35-tls-watch.log
    work/logs/roblox-native-34-system-ca.log
    work/logs/roblox-native-33-tls.log
    work/logs/roblox-native-32-tls.log
    work/logs/roblox-native-31.log
    work/logs/native-menu-cursors-primary-build.log
    work/logs/native-menu-build.log
    work/logs/native-fiber-build.log
    work/logs/native-errno-trace.log
    work/logs/native-errno-tests.log
    work/logs/native-errno-build.log
    work/logs/roblox-native-30-core-stack.log
    work/logs/roblox-native-29.log
    work/logs/roblox-native-28.log
    work/logs/roblox-native-27.log
    work/logs/roblox-native-26.log
    work/logs/roblox-native-25.log
    work/logs/roblox-native-24.log
    work/logs/roblox-native-23.log
    work/logs/roblox-native-22.log
    work/logs/roblox-native-21.log
    work/logs/roblox-native-20.log
    work/logs/roblox-native-19.log
    work/logs/roblox-native-18.log
    work/logs/roblox-native-17.log
    work/logs/roblox-native-16.log
    work/logs/roblox-native-15.log
    work/logs/roblox-mevk-geometry-fixed.log
    work/logs/grandperspective-geometry-live-before.log
    work/logs/grandperspective-geometry-live-after.log
    work/logs/grandperspective-geometry-before.log
    work/logs/grandperspective-geometry-after.log
    work/logs/native-smoke.log
    work/logs/native-smoke-04.log
    work/logs/native-smoke-03.log
    work/logs/native-smoke-02.log
    work/logs/native-signals-tests.log
    work/logs/native-signals-tests-unsandboxed.log
    work/logs/native-signals-build.log
    work/logs/native-passwd-tests.log
    work/logs/native-passwd-build.log
    work/logs/native-menu-windows.json
    work/logs/native-menu-software-demo.log
    work/logs/native-menu-primary-build.log
    work/logs/native-menu-opengl.png
    work/logs/native-menu-opengl-demo.log
    work/logs/grandperspective-geometry-build.log
    work/logs/live-nib-menu-cursors-native-keys.memory.csv
    work/logs/live-nib-menu-cursors-native-keys.log
    work/logs/live-nib-menu-cursors-native-keys.host-stack.log
    work/logs/live-native-menu.memory.csv
    work/logs/live-native-menu.log
    work/logs/live-native-menu.host-stack.log
    work/logs/cursors-native-keys-build.log
    work/logs/archive/live-before-signal-scope.log.gz
    work/logs/archive/live-after-signal-scope.log.gz
    work/live-window-geometry.json
    work/dump-nib-geometry
    work/preferences-nib-geometry.txt
    work/dump-nib-geometry.cpp
    work/trace-native-tls.cpp
    work/trace-native-tls
    work/start-nib-geometry.txt
    work/bootstrap-shell-downloads/sequoia-staging-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/htmldoc/Alternative-Completion.html
    work/base-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/htmldoc/Alternative-Completion.html
    work/bootstrap-shell-downloads/sequoia-staging-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_x_geometry
    work/bootstrap-shell-downloads/sequoia-staging-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_update-alternatives
    work/bootstrap-shell-downloads/sequoia-staging-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_signals
    work/bootstrap-shell-downloads/sequoia-staging-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_alternative
    work/install-build/tests/CMakeFiles/test_native_menu.dir/progress.make
    work/install-build/tests/CMakeFiles/test_native_menu.dir/link.txt
    work/install-build/tests/CMakeFiles/test_native_menu.dir/flags.make
    work/install-build/tests/CMakeFiles/test_native_menu.dir/DependInfo.cmake
    work/install-build/tests/CMakeFiles/test_native_menu.dir/depend.make
    work/install-build/tests/CMakeFiles/test_native_menu.dir/compiler_depend.ts
    work/install-build/tests/CMakeFiles/test_native_menu.dir/compiler_depend.make
    work/install-build/tests/CMakeFiles/test_native_menu.dir/cmake_clean.cmake
    work/install-build/tests/CMakeFiles/test_native_menu.dir/build.make
    work/base-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_x_geometry
    work/base-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_update-alternatives
    work/base-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_signals
    work/base-venv/opt/homebrew/Cellar/zsh/5.9.2/share/zsh/functions/_alternative
    work/native-menu-demo.m
    work/base-venv/opt/homebrew/Cellar/ncurses/6.6/share/terminfo/72/rxvt-cygwin-native
    work/bootstrap-shell-downloads/sequoia-staging-venv/opt/homebrew/Cellar/ncurses/6.6/share/terminfo/72/rxvt-cygwin-native
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/Helpers/Push3.app/Contents/Resources/python3_dist/site-packages/sentry-sdk-1.32.0/sentry_sdk/integrations/django/signals_handlers.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/Helpers/Push3.app/Contents/Resources/python3_dist/Lib/unittest/signals.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/Helpers/Push3.app/Contents/Resources/python3_dist/Lib/signal.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/Helpers/Push3.app/Contents/Resources/python/ableton/v2/base/signal.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Python/lib/unittest/signals.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Core Library/Samples/One Shots/Drums/Ride/Ride Bell Stick Alternative Rock.aif.asd
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Core Library/Samples/One Shots/Drums/Ride/Ride Bell Stick Alternative Rock.aif
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Python/lib/signal.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/mc/mc_signal_visualizers.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/legacy-examples/effects/signal-limiter/squirrelcomp-link.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/legacy-examples/effects/signal-limiter/squirrelcomp-help.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/legacy-examples/effects/signal-limiter/lib/squirrelcomp.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/pool-3d-nurbs.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.volume.movcube.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.volume-vidpillows.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.videoplane-xfade.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.videoplane-ortho.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.videoplane-multi.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.nurbs-video-deform.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.nurbs-ghosts.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.nurbs-closedblob.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.mesh.displace.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.mesh.bfg.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.isosurf-mov.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.isosurf-matrixoutput.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.isosurf-fortresses.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.gridshape-skittles.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.gridshape-scissors.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.gridshape-scanoffset.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.gridshape-plur.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/Examples/jitter-examples/render/geometry/jit.gl.gridshape-morph.maxpat
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Geometry/help/jit.geometry.maxhelp
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Geometry/externals/jit.geometry.mxo/Contents/_CodeSignature/CodeResources
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Geometry/externals/jit.geometry.mxo/Contents/PkgInfo
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Geometry/externals/jit.geometry.mxo/Contents/MacOS/jit.geometry
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Geometry/externals/jit.geometry.mxo/Contents/Info.plist
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Geometry/docs/jit.geometry.maxref.xml
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/help/msp/mc_signal_utilities.maxhelp
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/MIDI Remote Scripts/ableton/v3/control_surface/display/event_signal/__init__.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/MIDI Remote Scripts/ableton/v3/control_surface/display/event_signal/type_decl.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/MIDI Remote Scripts/ableton/v3/control_surface/display/event_signal/core.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/MIDI Remote Scripts/ableton/v2/base/abl_signal.pyc
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/tar/node_modules/mkdirp/lib/use-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/tar/node_modules/mkdirp/lib/mkdirp-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/signal-exit/signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/signal-exit/README.md
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/signal-exit/package.json
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/signal-exit/LICENSE.txt
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/signal-exit/index.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/package.json
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/LICENSE.txt
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/mjs/signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/mjs/package.json
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/mjs/index.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/mjs/browser.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/cjs/signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/cjs/package.json
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/cjs/index.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/signal-exit/dist/cjs/browser.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/protobufjs/cli/wrappers/closure.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/use-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/use-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/use-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/use-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/mkdirp-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/mkdirp-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/mkdirp-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/mjs/mkdirp-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/use-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/use-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/use-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/use-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/mkdirp-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/mkdirp-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/mkdirp-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/node-gyp/node_modules/mkdirp/dist/cjs/src/mkdirp-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/mkdirp/lib/use-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/mkdirp/lib/mkdirp-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/foreground-child/dist/esm/proxy-signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/foreground-child/dist/esm/all-signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/foreground-child/dist/commonjs/proxy-signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/foreground-child/dist/commonjs/all-signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/use-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/use-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/use-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/use-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/mkdirp-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/mkdirp-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/mkdirp-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/mjs/mkdirp-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/use-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/use-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/use-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/use-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/mkdirp-native.js.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/mkdirp-native.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/mkdirp-native.d.ts.map
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/cacache/node_modules/mkdirp/dist/cjs/src/mkdirp-native.d.ts
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/lodash/_nativeKeysIn.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/lodash/_nativeCreate.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/lodash/_nativeKeys.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/@npmcli/run-script/lib/signal-manager.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/@npmcli/arborist/lib/signals.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Node for Max/source/bin/npm/node_modules/@npmcli/arborist/lib/signal-handling.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Tools/code/jxs/geometry/gm.videopoint.jxs
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Tools/code/jxs/geometry/gm.solidpoint.jxs
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Tools/code/jxs/geometry/gm.normals.jxs
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Tools/code/jxs/geometry/gm.illumilines.jxs
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Tools/code/jxs/geometry/gm.bspline.jxs
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/Jitter Tools/code/jxs/geometry/gm.billboard.jxs
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signalsend.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signalreceive.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signaloutlet.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signalinlet.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signalgate.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signalforwarder.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/namedsignaloutlet.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/namedsignalinlet.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/objects/signaladder.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@rnbo/xam/lib/closure.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/caniuse-lite/data/features/native-filesystem-api.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@babel/compat-data/native-modules.js
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/packages/RNBO/server/node_modules/@babel/compat-data/data/native-modules.json
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/docs/userguide/content/mc/mc_signals_newobjects.json
    work/venv/Applications/Ableton Live 12 Trial.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/docs/userguide/content/jitter/geometry.json
    CMakeLists.txt:31:    src/runtime/native_x86_64.S
    CMakeLists.txt:125:        src/dylibs/Foundation/geometry.cpp
    CMakeLists.txt:157:        src/dylibs/AppKit/native_menu.cpp
    CMakeLists.txt:158:        src/dylibs/AppKit/native_dialog.cpp src/dylibs/AppKit/file_panels.cpp
    CMakeLists.txt:275:        # AppKit alias must be able to resolve its native Metal.so dependency.
    CMakeLists.txt:454:            COMMENT "Installing native x86_64 guest C++ runtimes")
    tests/CMakeLists.txt:66:add_executable(test_native_menu test_native_menu.cpp ../src/dylibs/AppKit/native_menu.cpp)
    tests/CMakeLists.txt:67:target_link_libraries(test_native_menu PRIVATE machoman_core)
    tests/CMakeLists.txt:69:    target_link_libraries(test_native_menu PRIVATE PkgConfig::MM_PANGO)
    tests/CMakeLists.txt:70:    target_compile_definitions(test_native_menu PRIVATE MM_APPKIT_PANGO=1)
    tests/CMakeLists.txt:72:add_test(NAME native_menu COMMAND test_native_menu)
    tests/CMakeLists.txt:356:        set(MM_GEOMETRY_BINARY "${MM_SMOKE_VENV}/bin/geometry-${MM_GEOMETRY_ARCH}")
    tests/CMakeLists.txt:360:                "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_geometry.m" -o "${MM_GEOMETRY_BINARY}"
    tests/CMakeLists.txt:361:            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_geometry.m VERBATIM)
    tests/CMakeLists.txt:362:        add_custom_target(guest_geometry_${MM_GEOMETRY_ARCH}_binary ALL DEPENDS "${MM_GEOMETRY_BINARY}")
    tests/CMakeLists.txt:363:        add_test(NAME guest_geometry_${MM_GEOMETRY_ARCH}
    tests/CMakeLists.txt:365:                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" geometry-${MM_GEOMETRY_ARCH})
    tests/CMakeLists.txt:366:        set_tests_properties(guest_geometry_${MM_GEOMETRY_ARCH} PROPERTIES TIMEOUT 20
    tests/CMakeLists.txt:369:    set(MM_NATIVE_BINARY "${MM_SMOKE_VENV}/bin/native-smoke")
    tests/CMakeLists.txt:381:            PASS_REGULAR_EXPRESSION "Guest errno assignments, cached pointers, native libc, number conversion and thread isolation passed")
    tests/CMakeLists.txt:462:            PASS_REGULAR_EXPRESSION "SDK pthread key width, thread identity, native GS, TLV lifetime and destructor iterations passed")
    tests/CMakeLists.txt:464:    set(MM_NATIVE_SIGNALS "${MM_SMOKE_VENV}/bin/native-signals")
    tests/CMakeLists.txt:468:            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_signals.c" -o "${MM_NATIVE_SIGNALS}"
    tests/CMakeLists.txt:469:        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_signals.c VERBATIM)
    tests/CMakeLists.txt:470:    add_custom_target(guest_native_signals_binary ALL DEPENDS "${MM_NATIVE_SIGNALS}")
    tests/CMakeLists.txt:471:    add_test(NAME guest_native_signals COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" native-signals)
    tests/CMakeLists.txt:472:    set_tests_properties(guest_native_signals PROPERTIES TIMEOUT 10
    tests/CMakeLists.txt:564:            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_smoke.c" -o "${MM_NATIVE_BINARY}"
    tests/CMakeLists.txt:565:        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_smoke.c VERBATIM)
    tests/CMakeLists.txt:566:    add_custom_target(guest_native_smoke_binary ALL DEPENDS "${MM_NATIVE_BINARY}")
    tests/CMakeLists.txt:567:    add_test(NAME guest_native_smoke COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" native-smoke)
    tests/CMakeLists.txt:568:    set_tests_properties(guest_native_smoke PROPERTIES TIMEOUT 20
    tests/CMakeLists.txt:569:        PASS_REGULAR_EXPRESSION "native x86_64 calls, callbacks, varargs, dlsym and TLS passed")
    tests/CMakeLists.txt:570:    set(MM_NATIVE_INTERPOSER "${MM_SMOKE_VENV}/usr/lib/libnative-interposer.dylib")
    tests/CMakeLists.txt:571:    set(MM_NATIVE_INTERPOSE "${MM_SMOKE_VENV}/bin/native-interpose")
    tests/CMakeLists.txt:575:            -Wl,-install_name,@rpath/libnative-interposer.dylib
    tests/CMakeLists.txt:576:            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_interposer.c" -o "${MM_NATIVE_INTERPOSER}"
    tests/CMakeLists.txt:577:        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_interposer.c VERBATIM)
    tests/CMakeLists.txt:582:            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_interpose.c" -o "${MM_NATIVE_INTERPOSE}"
    tests/CMakeLists.txt:583:        DEPENDS "${MM_NATIVE_INTERPOSER}" guest_native_interpose.c VERBATIM)
    tests/CMakeLists.txt:584:    add_custom_target(guest_native_interpose_binary ALL DEPENDS "${MM_NATIVE_INTERPOSE}")
    tests/CMakeLists.txt:585:    add_test(NAME guest_native_interpose COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" native-interpose)
    tests/CMakeLists.txt:586:    set_tests_properties(guest_native_interpose PROPERTIES TIMEOUT 20
    tests/CMakeLists.txt:587:        PASS_REGULAR_EXPRESSION "native interposition and original-function fallback passed")
    tests/CMakeLists.txt:668:    add_executable(test_native_dialog test_native_dialog.cpp ../src/dylibs/AppKit/native_dialog.cpp)
    tests/CMakeLists.txt:669:    target_compile_definitions(test_native_dialog PRIVATE MM_APPKIT_GIO=1)
    tests/CMakeLists.txt:670:    target_link_libraries(test_native_dialog PRIVATE PkgConfig::MM_GIO)
    tests/CMakeLists.txt:671:    add_test(NAME native_dialog COMMAND test_native_dialog)
    tests/CMakeLists.txt:672:    set_tests_properties(native_dialog PROPERTIES TIMEOUT 15)
    ✓ • 32.79s

## Assistant

В конце сессии мы исправляли ввод и клики в Roblox, а также падение после ввода пароля. Последняя правка касалась стека при обратном вызове из host-кода в Intel-код; её успели собрать, но ещё не проверили. Начну с этой проверки, затем запущу Roblox с исправлениями.

## Activity

    $ rg -n 'text.view|focus|NSLog' tests/CMakeLists.txt tests/guest_text_view.m tests/guest_native_geometry.m && sed -n '1,230p' tests/guest_text_view.m
    tests/CMakeLists.txt:237:        set(MM_TEXT_BINARY "${MM_SMOKE_VENV}/bin/text-view-${MM_FOCUS_ARCH}")
    tests/CMakeLists.txt:241:                "${CMAKE_CURRENT_SOURCE_DIR}/guest_text_view.m" -o "${MM_TEXT_BINARY}"
    tests/CMakeLists.txt:242:            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_text_view.m VERBATIM)
    tests/CMakeLists.txt:243:        add_custom_target(guest_text_view_${MM_FOCUS_ARCH}_binary ALL DEPENDS "${MM_TEXT_BINARY}")
    tests/CMakeLists.txt:244:        add_test(NAME guest_text_view_${MM_FOCUS_ARCH}
    tests/CMakeLists.txt:246:                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" text-view-${MM_FOCUS_ARCH})
    tests/CMakeLists.txt:247:        set_tests_properties(guest_text_view_${MM_FOCUS_ARCH} PROPERTIES TIMEOUT 20
    tests/CMakeLists.txt:249:        set(MM_FOCUS_BINARY "${MM_SMOKE_VENV}/bin/focus-reentrancy-${MM_FOCUS_ARCH}")
    tests/CMakeLists.txt:253:                "${CMAKE_CURRENT_SOURCE_DIR}/guest_focus_reentrancy.m" -o "${MM_FOCUS_BINARY}"
    tests/CMakeLists.txt:254:            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_focus_reentrancy.m VERBATIM)
    tests/CMakeLists.txt:255:        add_custom_target(guest_focus_reentrancy_${MM_FOCUS_ARCH}_binary ALL DEPENDS "${MM_FOCUS_BINARY}")
    tests/CMakeLists.txt:256:        add_test(NAME guest_focus_reentrancy_${MM_FOCUS_ARCH}
    tests/CMakeLists.txt:258:                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" focus-reentrancy-${MM_FOCUS_ARCH})
    tests/CMakeLists.txt:259:        set_tests_properties(guest_focus_reentrancy_${MM_FOCUS_ARCH} PROPERTIES TIMEOUT 20
    tests/CMakeLists.txt:260:            PASS_REGULAR_EXPRESSION "AppKit focus rollback and reentrant responder selection passed")
    tests/guest_text_view.m:6:    if (!condition) { fprintf(stderr, "text view: %s\n", message); exit(1); }
    tests/guest_text_view.m:112:            "window sends pointer events to rendering view while field editor has keyboard focus");
    #import <AppKit/AppKit.h>
    #include <stdio.h>
    #include <stdlib.h>
    
    static void check(BOOL condition, const char *message) {
        if (!condition) { fprintf(stderr, "text view: %s\n", message); exit(1); }
    }
    @interface KeyProbe : NSEvent { @public NSEventType probeType; unsigned short code; NSEventModifierFlags flags; }
    @end
    @implementation KeyProbe
    - (NSEventType)type { return probeType; }
    - (NSString *)characters { return @"z"; }
    - (unsigned short)keyCode { return code; }
    - (NSEventModifierFlags)modifierFlags { return flags; }
    @end
    @interface PointerProbe : NSView { @public unsigned downs, ups, moves; }
    @end
    @implementation PointerProbe
    - (void)mouseDown:(NSEvent *)event { ++downs; }
    - (void)mouseUp:(NSEvent *)event { ++ups; }
    - (void)mouseMoved:(NSEvent *)event { ++moves; }
    @end
    @interface TextDelegate : NSObject <NSTextViewDelegate> { @public unsigned selections, changes; NSString *lastText; BOOL marked; }
    @end
    @implementation TextDelegate
    - (void)textViewDidChangeSelection:(NSNotification *)notification {
        ++selections;
        NSTextView *view = notification.object;
        [lastText release]; lastText = [view.string copy]; marked = view.hasMarkedText;
    }
    - (void)textDidChange:(NSNotification *)notification { ++changes; }
    - (void)dealloc { [lastText release]; [super dealloc]; }
    @end
    @interface FieldProbe : NSTextView { @public unsigned insertions; }
    @end
    @implementation FieldProbe
    - (void)insertText:(id)text replacementRange:(NSRange)range {
        ++insertions;
        [super insertText:text replacementRange:range];
    }
    @end
    int main(void) {
        @autoreleasepool {
            FieldProbe *editor = [[FieldProbe alloc] initWithFrame:NSMakeRect(5, 10, 150, 90)];
            check([editor isKindOfClass:[NSView class]] && editor.frame.size.width == 150, "view superclass and frame ABI");
            check([editor.string isEqualToString:@""] && editor.selectedRange.location == 0 &&
                editor.selectedRange.length == 0 && editor.markedRange.location == NSNotFound &&
                editor.markedRange.length == 0 && !editor.hasMarkedText, "initial string and range return ABI");
            [editor setFieldEditor:YES];
            check(editor.isFieldEditor, "field editor state");
            editor.string = @"abc";
            editor.selectedRange = NSMakeRange(1, 2);
            [editor insertText:@"x" replacementRange:NSMakeRange(NSNotFound, 0)];
            check([editor.string isEqualToString:@"ax"] && editor.selectedRange.location == 2, "selection replacement");
            KeyProbe *event = [KeyProbe new];
            event->probeType = NSEventTypeKeyDown; event->code = 6;
            [editor keyDown:event];
            check(editor->insertions == 2 && [editor.string isEqualToString:@"axz"], "keyDown invokes guest insertText override and super");
            TextDelegate *delegate = [TextDelegate new];
            editor.delegate = delegate;
            [editor keyDown:event];
            check(delegate->selections == 1 && delegate->changes == 1 &&
                [delegate->lastText isEqualToString:@"axzz"] && !delegate->marked,
                "each keystroke notifies the delegate with committed text before keyDown returns");
            event->probeType = NSEventTypeFlagsChanged; event->code = 57;
            [editor interpretKeyEvents:@[event]];
            event->probeType = NSEventTypeKeyDown; event->code = 59;
            [editor interpretKeyEvents:@[event]];
            event->code = 6; event->flags = NSEventModifierFlagControl;
            [editor interpretKeyEvents:@[event]];
            event->flags = NSEventModifierFlagCommand;
            [editor interpretKeyEvents:@[event]];
            event->flags = 0; event->code = 48;
            [editor interpretKeyEvents:@[event]];
            check([editor.string isEqualToString:@"axzz"] && delegate->changes == 1,
                "modifier events, Control/Command shortcuts and field-editor Tab do not insert text");
            editor.string = @"axz";
            [editor replaceCharactersInRange:NSMakeRange(0, editor.string.length) withString:@""];
            check(editor.string.length == 0 && editor.selectedRange.location == 0,
                "composition cleanup replaces the NSText buffer through its NSRange ABI");
            editor.string = @"axz";
            editor.selectedRange = NSMakeRange(3, 0);
            [editor setMarkedText:@"你好" selectedRange:NSMakeRange(1, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
            check(editor.hasMarkedText && editor.markedRange.location == 3 && editor.markedRange.length == 2 &&
                editor.selectedRange.location == 4 && editor.selectedRange.length == 1, "marked text and selected ranges");
            check(delegate->marked && [delegate->lastText isEqualToString:@"axz你好"], "composition callback sees final marked state");
            [editor insertText:@"!" replacementRange:NSMakeRange(NSNotFound, 0)];
            check([editor.string isEqualToString:@"axz!"] && !editor.hasMarkedText, "committing marked text");
            editor.string = @"😀";
            editor.selectedRange = NSMakeRange(2, 0);
            [editor deleteBackward:nil];
            check(editor.string.length == 0 && editor.selectedRange.location == 0, "delete UTF-16 surrogate pair");
            editor.string = @"ab";
            editor.selectedRange = NSMakeRange(0, 1);
            [editor setMarkedText:@"x" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
            [editor setMarkedText:@"yz" selectedRange:NSMakeRange(2, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
            check([editor.string isEqualToString:@"yzb"] && editor.markedRange.length == 2, "replace existing composition");
            [editor unmarkText];
            NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 300, 200)
                styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
            NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
            PointerProbe *render = [[PointerProbe alloc] initWithFrame:root.bounds];
            NSTextView *zeroEditor = [[NSTextView alloc] initWithFrame:NSZeroRect];
            [root addSubview:render]; [render addSubview:zeroEditor]; window.contentView = root;
            [window makeFirstResponder:zeroEditor];
            check([root hitTest:NSMakePoint(90, 70)] == render,
                "zero-sized field editor does not intercept pointer hit testing");
            event->probeType = NSEventTypeMouseMoved; [window sendEvent:event];
            event->probeType = NSEventTypeLeftMouseDown; [window sendEvent:event];
            event->probeType = NSEventTypeLeftMouseUp; [window sendEvent:event];
            check(render->moves == 1 && render->downs == 1 && render->ups == 1,
                "window sends pointer events to rendering view while field editor has keyboard focus");
            NSStackView *container = [NSStackView stackViewWithViews:@[[NSView new]]];
            NSBox *box = [NSBox new];
            [box addSubview:container]; [root addSubview:box];
            check([container isKindOfClass:NSView.class] && container.window == window &&
                container.arrangedSubviews.count == 1 && ((NSView *)container.arrangedSubviews[0]).window == window,
                "stack containers have a real view payload and recursively inherit window membership");
            [window makeFirstResponder:nil]; [zeroEditor release]; [render release]; [root release];
            [box release]; [window release];
            editor.delegate = nil;
            [delegate release]; [event release]; [editor release];
        }
        puts("NSTextView field editor, guest text callbacks and UTF-16 range ABI passed");
        return 0;
    }
    ✓ • 0ms

## Activity

    $ python3 -c 'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text().splitlines(); matches=[i for i,l in enumerate(s) if "roblox-input-build/machoman run" in l or "roblox-login" in l or "roblox-input" in l and "python3" in l]; print("\n".join("\n".join(s[max(0,i-2):i+4]) for i in matches[-16:]))' && pgrep -af 'machoman|RobloxPlayer'
        [Thread debugging using libthread_db enabled]
        Using host libthread_db library "/usr/lib/libthread_db.so.1".
        Core was generated by `/tmp/machoman-roblox-input-build/machoman run /mnt/media/machoman/work/venv RobloxPlayer'.
        Program terminated with signal SIGSEGV, Segmentation fault.
        #0  0x00007fbab649a11c in ?? () from /usr/lib/libc.so.6
        [Current thread is 1 (Thread 0x7fbab6b40840 (LWP 193152))]
                Signal: 11 (SEGV) si_code: SI_TKILL
             Timestamp: Fri 2026-11-13 04:25:15 MSK (9min ago)
          Command Line: /tmp/machoman-roblox-input-build/machoman run /mnt/media/machoman/work/venv RobloxPlayer
            Executable: /tmp/machoman-roblox-input-build/machoman
         Control Group: /user.slice/user-1000.slice/user@1000.service/app.slice/app-com.microsoft.VSCode-134805.scope
                  Unit: user@1000.service
        [Thread debugging using libthread_db enabled]
        Using host libthread_db library "/usr/lib/libthread_db.so.1".
        Core was generated by `/tmp/machoman-roblox-input-build/machoman run /mnt/media/machoman/work/venv RobloxPlayer'.
        Program terminated with signal SIGSEGV, Segmentation fault.
        #0  0x00007fbab649a11c in ?? () from /usr/lib/libc.so.6
        [Current thread is 1 (Thread 0x7fbab6b40840 (LWP 193152))]
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Make breakpoint pending on future shared library load? (y or [n]) [answered N; input not from terminal]
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
        work/text-view-test.gdb:7: Error in sourced command file:
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        work/text-view-test.gdb:9: Error in sourced command file:
        malformed linespec error: unexpected string, "== 0"
        ✗ (1) • 123ms
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Breakpoint 1 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Breakpoint 2 ('ak::(anonymous namespace)::text_selection_changed') pending.
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Breakpoint 1 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
        Breakpoint 2 ('ak::(anonymous namespace)::text_selection_changed') pending.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Breakpoint 1 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
        Breakpoint 2 ('ak::(anonymous namespace)::text_selection_changed') pending.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Breakpoint 3 ('ak::(anonymous namespace)::text_selection_changed') pending.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Breakpoint 3 ('ak::(anonymous namespace)::text_selection_changed') pending.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-handle-watch.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Breakpoint 1 at 0xaad3d: file /mnt/media/machoman/src/runtime/loader.cpp, line 1884.
        [Thread debugging using libthread_db enabled]
        Using host libthread_db library "/usr/lib/libthread_db.so.1".
    ## Activity
    
        $ env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
        /usr/bin/bash: line 1:     3 Segmentation fault         (core dumped) env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
        ✗ (139) • 87ms
    
    
        $ env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
        /usr/bin/bash: line 1:     3 Segmentation fault         (core dumped) env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
        ✗ (139) • 87ms
    
    ## Activity
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
        Breakpoint 3 ('ak::(anonymous namespace)::text_selection_changed') pending.
    1 codex-linux-sandbox --sandbox-policy-cwd /mnt/media/machoman --command-cwd /mnt/media/machoman --permission-profile {"type":"managed","file_system":{"type":"restricted","entries":[{"path":{"type":"special","value":{"kind":"root"}},"access":"read"},{"path":{"type":"path","path":"/mnt/media/machoman"},"access":"write"},{"path":{"type":"special","value":{"kind":"slash_tmp"}},"access":"write"},{"path":{"type":"special","value":{"kind":"tmpdir"}},"access":"write"},{"path":{"type":"path","path":"/mnt/media/machoman/.git"},"access":"read","missing_path_behavior":"skip"},{"path":{"type":"path","path":"/mnt/media/machoman/.agents"},"access":"read","missing_path_behavior":"skip"},{"path":{"type":"path","path":"/mnt/media/machoman/.codex"},"access":"read","missing_path_behavior":"skip"},{"path":{"type":"path","path":"/mnt/media/machoman/.aws"},"access":"read","missing_path_behavior":"skip"}]},"network":"restricted"} --apply-seccomp-then-exec -- /usr/bin/bash -c __CODEX_SNAPSHOT_OVERRIDE_SET_0="${CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS+x}" __CODEX_SNAPSHOT_OVERRIDE_0="${CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS-}" __CODEX_SNAPSHOT_OVERRIDE_SET_1="${CODEX_PERMISSION_PROFILE+x}" __CODEX_SNAPSHOT_OVERRIDE_1="${CODEX_PERMISSION_PROFILE-}" __CODEX_SNAPSHOT_OVERRIDE_SET_2="${CODEX_PLUGIN_METRICS_OUTPUT+x}" __CODEX_SNAPSHOT_OVERRIDE_2="${CODEX_PLUGIN_METRICS_OUTPUT-}" __CODEX_SNAPSHOT_OVERRIDE_SET_3="${CODEX_SESSION_ID+x}" __CODEX_SNAPSHOT_OVERRIDE_3="${CODEX_SESSION_ID-}" __CODEX_SNAPSHOT_OVERRIDE_SET_4="${CODEX_THREAD_ID+x}" __CODEX_SNAPSHOT_OVERRIDE_4="${CODEX_THREAD_ID-}" __CODEX_SNAPSHOT_OVERRIDE_SET_5="${CODEX_TOOL_CALL_ID+x}" __CODEX_SNAPSHOT_OVERRIDE_5="${CODEX_TOOL_CALL_ID-}" __CODEX_SNAPSHOT_OVERRIDE_SET_6="${CODEX_VERSION+x}" __CODEX_SNAPSHOT_OVERRIDE_6="${CODEX_VERSION-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_0="${ALL_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_0="${ALL_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_1="${BUNDLE_HTTPS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_1="${BUNDLE_HTTPS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_2="${BUNDLE_HTTP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_2="${BUNDLE_HTTP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_3="${BUNDLE_NO_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_3="${BUNDLE_NO_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_4="${BUNDLE_SSL_CA_CERT+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_4="${BUNDLE_SSL_CA_CERT-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_5="${CARGO_HTTP_CAINFO+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_5="${CARGO_HTTP_CAINFO-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_6="${CODEX_CA_CERTIFICATE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_6="${CODEX_CA_CERTIFICATE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_7="${CODEX_NETWORK_ALLOW_LOCAL_BINDING+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_7="${CODEX_NETWORK_ALLOW_LOCAL_BINDING-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_8="${CODEX_NETWORK_PROXY_ACTIVE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_8="${CODEX_NETWORK_PROXY_ACTIVE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_9="${CODEX_NETWORK_PROXY_ATTRIBUTION+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_9="${CODEX_NETWORK_PROXY_ATTRIBUTION-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_10="${CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_10="${CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_11="${CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_11="${CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_12="${CURL_CA_BUNDLE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_12="${CURL_CA_BUNDLE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_13="${DOCKER_HTTPS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_13="${DOCKER_HTTPS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_14="${DOCKER_HTTP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_14="${DOCKER_HTTP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_15="${ELECTRON_GET_USE_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_15="${ELECTRON_GET_USE_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_16="${FTP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_16="${FTP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_17="${GIT_SSL_CAINFO+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_17="${GIT_SSL_CAINFO-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_18="${HTTPS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_18="${HTTPS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_19="${HTTP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_19="${HTTP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_20="${NODE_EXTRA_CA_CERTS+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_20="${NODE_EXTRA_CA_CERTS-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_21="${NODE_USE_ENV_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_21="${NODE_USE_ENV_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_22="${NO_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_22="${NO_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_23="${NPM_CONFIG_CAFILE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_23="${NPM_CONFIG_CAFILE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_24="${NPM_CONFIG_HTTPS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_24="${NPM_CONFIG_HTTPS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_25="${NPM_CONFIG_HTTP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_25="${NPM_CONFIG_HTTP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_26="${NPM_CONFIG_NOPROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_26="${NPM_CONFIG_NOPROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_27="${NPM_CONFIG_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_27="${NPM_CONFIG_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_28="${PIP_CERT+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_28="${PIP_CERT-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_29="${PIP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_29="${PIP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_30="${REQUESTS_CA_BUNDLE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_30="${REQUESTS_CA_BUNDLE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_31="${SSL_CERT_FILE+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_31="${SSL_CERT_FILE-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_32="${WSS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_32="${WSS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_33="${WS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_33="${WS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_34="${YARN_HTTPS_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_34="${YARN_HTTPS_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_35="${YARN_HTTP_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_35="${YARN_HTTP_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_36="${YARN_NO_PROXY+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_36="${YARN_NO_PROXY-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_37="${all_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_37="${all_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_38="${ftp_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_38="${ftp_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_39="${http_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_39="${http_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_40="${https_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_40="${https_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_41="${no_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_41="${no_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_42="${npm_config_cafile+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_42="${npm_config_cafile-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_43="${npm_config_http_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_43="${npm_config_http_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_44="${npm_config_https_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_44="${npm_config_https_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_45="${npm_config_noproxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_45="${npm_config_noproxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_46="${npm_config_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_46="${npm_config_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_47="${ws_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_47="${ws_proxy-}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_48="${wss_proxy+x}" __CODEX_SNAPSHOT_PROXY_OVERRIDE_48="${wss_proxy-}" __CODEX_SNAPSHOT_PROXY_ENV_SET="${CODEX_NETWORK_PROXY_ACTIVE+x}"  if . '/home/monsler/.codex/shell_snapshots/01a1d29e-3e8d-74f1-9e22-c10e09970e10.1794534948511428312.sh' >/dev/null 2>&1; then :; fi  if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_0}" ]; then   if [ -z "${CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS+x}" ] || [ "${CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS-}" != "${__CODEX_SNAPSHOT_OVERRIDE_0}" ]; then export CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS="${__CODEX_SNAPSHOT_OVERRIDE_0}"; else export CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS; fi else builtin unset CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS 2>/dev/null || command unset CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_0 __CODEX_SNAPSHOT_OVERRIDE_0 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_0 __CODEX_SNAPSHOT_OVERRIDE_0 if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_1}" ]; then   if [ -z "${CODEX_PERMISSION_PROFILE+x}" ] || [ "${CODEX_PERMISSION_PROFILE-}" != "${__CODEX_SNAPSHOT_OVERRIDE_1}" ]; then export CODEX_PERMISSION_PROFILE="${__CODEX_SNAPSHOT_OVERRIDE_1}"; else export CODEX_PERMISSION_PROFILE; fi else builtin unset CODEX_PERMISSION_PROFILE 2>/dev/null || command unset CODEX_PERMISSION_PROFILE; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_1 __CODEX_SNAPSHOT_OVERRIDE_1 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_1 __CODEX_SNAPSHOT_OVERRIDE_1 if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_2}" ]; then   if [ -z "${CODEX_PLUGIN_METRICS_OUTPUT+x}" ] || [ "${CODEX_PLUGIN_METRICS_OUTPUT-}" != "${__CODEX_SNAPSHOT_OVERRIDE_2}" ]; then export CODEX_PLUGIN_METRICS_OUTPUT="${__CODEX_SNAPSHOT_OVERRIDE_2}"; else export CODEX_PLUGIN_METRICS_OUTPUT; fi else builtin unset CODEX_PLUGIN_METRICS_OUTPUT 2>/dev/null || command unset CODEX_PLUGIN_METRICS_OUTPUT; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_2 __CODEX_SNAPSHOT_OVERRIDE_2 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_2 __CODEX_SNAPSHOT_OVERRIDE_2 if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_3}" ]; then   if [ -z "${CODEX_SESSION_ID+x}" ] || [ "${CODEX_SESSION_ID-}" != "${__CODEX_SNAPSHOT_OVERRIDE_3}" ]; then export CODEX_SESSION_ID="${__CODEX_SNAPSHOT_OVERRIDE_3}"; else export CODEX_SESSION_ID; fi else builtin unset CODEX_SESSION_ID 2>/dev/null || command unset CODEX_SESSION_ID; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_3 __CODEX_SNAPSHOT_OVERRIDE_3 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_3 __CODEX_SNAPSHOT_OVERRIDE_3 if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_4}" ]; then   if [ -z "${CODEX_THREAD_ID+x}" ] || [ "${CODEX_THREAD_ID-}" != "${__CODEX_SNAPSHOT_OVERRIDE_4}" ]; then export CODEX_THREAD_ID="${__CODEX_SNAPSHOT_OVERRIDE_4}"; else export CODEX_THREAD_ID; fi else builtin unset CODEX_THREAD_ID 2>/dev/null || command unset CODEX_THREAD_ID; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_4 __CODEX_SNAPSHOT_OVERRIDE_4 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_4 __CODEX_SNAPSHOT_OVERRIDE_4 if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_5}" ]; then   if [ -z "${CODEX_TOOL_CALL_ID+x}" ] || [ "${CODEX_TOOL_CALL_ID-}" != "${__CODEX_SNAPSHOT_OVERRIDE_5}" ]; then export CODEX_TOOL_CALL_ID="${__CODEX_SNAPSHOT_OVERRIDE_5}"; else export CODEX_TOOL_CALL_ID; fi else builtin unset CODEX_TOOL_CALL_ID 2>/dev/null || command unset CODEX_TOOL_CALL_ID; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_5 __CODEX_SNAPSHOT_OVERRIDE_5 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_5 __CODEX_SNAPSHOT_OVERRIDE_5 if [ -n "${__CODEX_SNAPSHOT_OVERRIDE_SET_6}" ]; then   if [ -z "${CODEX_VERSION+x}" ] || [ "${CODEX_VERSION-}" != "${__CODEX_SNAPSHOT_OVERRIDE_6}" ]; then export CODEX_VERSION="${__CODEX_SNAPSHOT_OVERRIDE_6}"; else export CODEX_VERSION; fi else builtin unset CODEX_VERSION 2>/dev/null || command unset CODEX_VERSION; fi builtin unset __CODEX_SNAPSHOT_OVERRIDE_SET_6 __CODEX_SNAPSHOT_OVERRIDE_6 2>/dev/null || command unset __CODEX_SNAPSHOT_OVERRIDE_SET_6 __CODEX_SNAPSHOT_OVERRIDE_6 if [ -n "$__CODEX_SNAPSHOT_PROXY_ENV_SET" ] || [ -n "${CODEX_NETWORK_PROXY_ACTIVE+x}" ]; then if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_0}" ]; then   if [ -z "${ALL_PROXY+x}" ] || [ "${ALL_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_0}" ]; then export ALL_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_0}"; else export ALL_PROXY; fi else builtin unset ALL_PROXY 2>/dev/null || command unset ALL_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_0 __CODEX_SNAPSHOT_PROXY_OVERRIDE_0 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_0 __CODEX_SNAPSHOT_PROXY_OVERRIDE_0 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_1}" ]; then   if [ -z "${BUNDLE_HTTPS_PROXY+x}" ] || [ "${BUNDLE_HTTPS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_1}" ]; then export BUNDLE_HTTPS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_1}"; else export BUNDLE_HTTPS_PROXY; fi else builtin unset BUNDLE_HTTPS_PROXY 2>/dev/null || command unset BUNDLE_HTTPS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_1 __CODEX_SNAPSHOT_PROXY_OVERRIDE_1 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_1 __CODEX_SNAPSHOT_PROXY_OVERRIDE_1 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_2}" ]; then   if [ -z "${BUNDLE_HTTP_PROXY+x}" ] || [ "${BUNDLE_HTTP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_2}" ]; then export BUNDLE_HTTP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_2}"; else export BUNDLE_HTTP_PROXY; fi else builtin unset BUNDLE_HTTP_PROXY 2>/dev/null || command unset BUNDLE_HTTP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_2 __CODEX_SNAPSHOT_PROXY_OVERRIDE_2 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_2 __CODEX_SNAPSHOT_PROXY_OVERRIDE_2 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_3}" ]; then   if [ -z "${BUNDLE_NO_PROXY+x}" ] || [ "${BUNDLE_NO_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_3}" ]; then export BUNDLE_NO_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_3}"; else export BUNDLE_NO_PROXY; fi else builtin unset BUNDLE_NO_PROXY 2>/dev/null || command unset BUNDLE_NO_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_3 __CODEX_SNAPSHOT_PROXY_OVERRIDE_3 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_3 __CODEX_SNAPSHOT_PROXY_OVERRIDE_3 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_4}" ]; then   if [ -z "${BUNDLE_SSL_CA_CERT+x}" ] || [ "${BUNDLE_SSL_CA_CERT-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_4}" ]; then export BUNDLE_SSL_CA_CERT="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_4}"; else export BUNDLE_SSL_CA_CERT; fi else builtin unset BUNDLE_SSL_CA_CERT 2>/dev/null || command unset BUNDLE_SSL_CA_CERT; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_4 __CODEX_SNAPSHOT_PROXY_OVERRIDE_4 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_4 __CODEX_SNAPSHOT_PROXY_OVERRIDE_4 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_5}" ]; then   if [ -z "${CARGO_HTTP_CAINFO+x}" ] || [ "${CARGO_HTTP_CAINFO-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_5}" ]; then export CARGO_HTTP_CAINFO="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_5}"; else export CARGO_HTTP_CAINFO; fi else builtin unset CARGO_HTTP_CAINFO 2>/dev/null || command unset CARGO_HTTP_CAINFO; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_5 __CODEX_SNAPSHOT_PROXY_OVERRIDE_5 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_5 __CODEX_SNAPSHOT_PROXY_OVERRIDE_5 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_6}" ]; then   if [ -z "${CODEX_CA_CERTIFICATE+x}" ] || [ "${CODEX_CA_CERTIFICATE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_6}" ]; then export CODEX_CA_CERTIFICATE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_6}"; else export CODEX_CA_CERTIFICATE; fi else builtin unset CODEX_CA_CERTIFICATE 2>/dev/null || command unset CODEX_CA_CERTIFICATE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_6 __CODEX_SNAPSHOT_PROXY_OVERRIDE_6 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_6 __CODEX_SNAPSHOT_PROXY_OVERRIDE_6 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_7}" ]; then   if [ -z "${CODEX_NETWORK_ALLOW_LOCAL_BINDING+x}" ] || [ "${CODEX_NETWORK_ALLOW_LOCAL_BINDING-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_7}" ]; then export CODEX_NETWORK_ALLOW_LOCAL_BINDING="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_7}"; else export CODEX_NETWORK_ALLOW_LOCAL_BINDING; fi else builtin unset CODEX_NETWORK_ALLOW_LOCAL_BINDING 2>/dev/null || command unset CODEX_NETWORK_ALLOW_LOCAL_BINDING; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_7 __CODEX_SNAPSHOT_PROXY_OVERRIDE_7 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_7 __CODEX_SNAPSHOT_PROXY_OVERRIDE_7 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_8}" ]; then   if [ -z "${CODEX_NETWORK_PROXY_ACTIVE+x}" ] || [ "${CODEX_NETWORK_PROXY_ACTIVE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_8}" ]; then export CODEX_NETWORK_PROXY_ACTIVE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_8}"; else export CODEX_NETWORK_PROXY_ACTIVE; fi else builtin unset CODEX_NETWORK_PROXY_ACTIVE 2>/dev/null || command unset CODEX_NETWORK_PROXY_ACTIVE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_8 __CODEX_SNAPSHOT_PROXY_OVERRIDE_8 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_8 __CODEX_SNAPSHOT_PROXY_OVERRIDE_8 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_9}" ]; then   if [ -z "${CODEX_NETWORK_PROXY_ATTRIBUTION+x}" ] || [ "${CODEX_NETWORK_PROXY_ATTRIBUTION-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_9}" ]; then export CODEX_NETWORK_PROXY_ATTRIBUTION="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_9}"; else export CODEX_NETWORK_PROXY_ATTRIBUTION; fi else builtin unset CODEX_NETWORK_PROXY_ATTRIBUTION 2>/dev/null || command unset CODEX_NETWORK_PROXY_ATTRIBUTION; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_9 __CODEX_SNAPSHOT_PROXY_OVERRIDE_9 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_9 __CODEX_SNAPSHOT_PROXY_OVERRIDE_9 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_10}" ]; then   if [ -z "${CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS+x}" ] || [ "${CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_10}" ]; then export CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_10}"; else export CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS; fi else builtin unset CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS 2>/dev/null || command unset CODEX_NETWORK_PROXY_BROKERED_CREDENTIALS; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_10 __CODEX_SNAPSHOT_PROXY_OVERRIDE_10 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_10 __CODEX_SNAPSHOT_PROXY_OVERRIDE_10 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_11}" ]; then   if [ -z "${CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE+x}" ] || [ "${CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_11}" ]; then export CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_11}"; else export CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE; fi else builtin unset CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE 2>/dev/null || command unset CODEX_NETWORK_PROXY_CREDENTIAL_BROKER_ACTIVE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_11 __CODEX_SNAPSHOT_PROXY_OVERRIDE_11 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_11 __CODEX_SNAPSHOT_PROXY_OVERRIDE_11 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_12}" ]; then   if [ -z "${CURL_CA_BUNDLE+x}" ] || [ "${CURL_CA_BUNDLE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_12}" ]; then export CURL_CA_BUNDLE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_12}"; else export CURL_CA_BUNDLE; fi else builtin unset CURL_CA_BUNDLE 2>/dev/null || command unset CURL_CA_BUNDLE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_12 __CODEX_SNAPSHOT_PROXY_OVERRIDE_12 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_12 __CODEX_SNAPSHOT_PROXY_OVERRIDE_12 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_13}" ]; then   if [ -z "${DOCKER_HTTPS_PROXY+x}" ] || [ "${DOCKER_HTTPS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_13}" ]; then export DOCKER_HTTPS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_13}"; else export DOCKER_HTTPS_PROXY; fi else builtin unset DOCKER_HTTPS_PROXY 2>/dev/null || command unset DOCKER_HTTPS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_13 __CODEX_SNAPSHOT_PROXY_OVERRIDE_13 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_13 __CODEX_SNAPSHOT_PROXY_OVERRIDE_13 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_14}" ]; then   if [ -z "${DOCKER_HTTP_PROXY+x}" ] || [ "${DOCKER_HTTP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_14}" ]; then export DOCKER_HTTP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_14}"; else export DOCKER_HTTP_PROXY; fi else builtin unset DOCKER_HTTP_PROXY 2>/dev/null || command unset DOCKER_HTTP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_14 __CODEX_SNAPSHOT_PROXY_OVERRIDE_14 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_14 __CODEX_SNAPSHOT_PROXY_OVERRIDE_14 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_15}" ]; then   if [ -z "${ELECTRON_GET_USE_PROXY+x}" ] || [ "${ELECTRON_GET_USE_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_15}" ]; then export ELECTRON_GET_USE_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_15}"; else export ELECTRON_GET_USE_PROXY; fi else builtin unset ELECTRON_GET_USE_PROXY 2>/dev/null || command unset ELECTRON_GET_USE_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_15 __CODEX_SNAPSHOT_PROXY_OVERRIDE_15 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_15 __CODEX_SNAPSHOT_PROXY_OVERRIDE_15 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_16}" ]; then   if [ -z "${FTP_PROXY+x}" ] || [ "${FTP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_16}" ]; then export FTP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_16}"; else export FTP_PROXY; fi else builtin unset FTP_PROXY 2>/dev/null || command unset FTP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_16 __CODEX_SNAPSHOT_PROXY_OVERRIDE_16 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_16 __CODEX_SNAPSHOT_PROXY_OVERRIDE_16 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_17}" ]; then   if [ -z "${GIT_SSL_CAINFO+x}" ] || [ "${GIT_SSL_CAINFO-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_17}" ]; then export GIT_SSL_CAINFO="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_17}"; else export GIT_SSL_CAINFO; fi else builtin unset GIT_SSL_CAINFO 2>/dev/null || command unset GIT_SSL_CAINFO; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_17 __CODEX_SNAPSHOT_PROXY_OVERRIDE_17 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_17 __CODEX_SNAPSHOT_PROXY_OVERRIDE_17 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_18}" ]; then   if [ -z "${HTTPS_PROXY+x}" ] || [ "${HTTPS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_18}" ]; then export HTTPS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_18}"; else export HTTPS_PROXY; fi else builtin unset HTTPS_PROXY 2>/dev/null || command unset HTTPS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_18 __CODEX_SNAPSHOT_PROXY_OVERRIDE_18 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_18 __CODEX_SNAPSHOT_PROXY_OVERRIDE_18 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_19}" ]; then   if [ -z "${HTTP_PROXY+x}" ] || [ "${HTTP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_19}" ]; then export HTTP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_19}"; else export HTTP_PROXY; fi else builtin unset HTTP_PROXY 2>/dev/null || command unset HTTP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_19 __CODEX_SNAPSHOT_PROXY_OVERRIDE_19 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_19 __CODEX_SNAPSHOT_PROXY_OVERRIDE_19 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_20}" ]; then   if [ -z "${NODE_EXTRA_CA_CERTS+x}" ] || [ "${NODE_EXTRA_CA_CERTS-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_20}" ]; then export NODE_EXTRA_CA_CERTS="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_20}"; else export NODE_EXTRA_CA_CERTS; fi else builtin unset NODE_EXTRA_CA_CERTS 2>/dev/null || command unset NODE_EXTRA_CA_CERTS; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_20 __CODEX_SNAPSHOT_PROXY_OVERRIDE_20 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_20 __CODEX_SNAPSHOT_PROXY_OVERRIDE_20 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_21}" ]; then   if [ -z "${NODE_USE_ENV_PROXY+x}" ] || [ "${NODE_USE_ENV_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_21}" ]; then export NODE_USE_ENV_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_21}"; else export NODE_USE_ENV_PROXY; fi else builtin unset NODE_USE_ENV_PROXY 2>/dev/null || command unset NODE_USE_ENV_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_21 __CODEX_SNAPSHOT_PROXY_OVERRIDE_21 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_21 __CODEX_SNAPSHOT_PROXY_OVERRIDE_21 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_22}" ]; then   if [ -z "${NO_PROXY+x}" ] || [ "${NO_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_22}" ]; then export NO_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_22}"; else export NO_PROXY; fi else builtin unset NO_PROXY 2>/dev/null || command unset NO_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_22 __CODEX_SNAPSHOT_PROXY_OVERRIDE_22 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_22 __CODEX_SNAPSHOT_PROXY_OVERRIDE_22 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_23}" ]; then   if [ -z "${NPM_CONFIG_CAFILE+x}" ] || [ "${NPM_CONFIG_CAFILE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_23}" ]; then export NPM_CONFIG_CAFILE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_23}"; else export NPM_CONFIG_CAFILE; fi else builtin unset NPM_CONFIG_CAFILE 2>/dev/null || command unset NPM_CONFIG_CAFILE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_23 __CODEX_SNAPSHOT_PROXY_OVERRIDE_23 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_23 __CODEX_SNAPSHOT_PROXY_OVERRIDE_23 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_24}" ]; then   if [ -z "${NPM_CONFIG_HTTPS_PROXY+x}" ] || [ "${NPM_CONFIG_HTTPS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_24}" ]; then export NPM_CONFIG_HTTPS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_24}"; else export NPM_CONFIG_HTTPS_PROXY; fi else builtin unset NPM_CONFIG_HTTPS_PROXY 2>/dev/null || command unset NPM_CONFIG_HTTPS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_24 __CODEX_SNAPSHOT_PROXY_OVERRIDE_24 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_24 __CODEX_SNAPSHOT_PROXY_OVERRIDE_24 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_25}" ]; then   if [ -z "${NPM_CONFIG_HTTP_PROXY+x}" ] || [ "${NPM_CONFIG_HTTP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_25}" ]; then export NPM_CONFIG_HTTP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_25}"; else export NPM_CONFIG_HTTP_PROXY; fi else builtin unset NPM_CONFIG_HTTP_PROXY 2>/dev/null || command unset NPM_CONFIG_HTTP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_25 __CODEX_SNAPSHOT_PROXY_OVERRIDE_25 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_25 __CODEX_SNAPSHOT_PROXY_OVERRIDE_25 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_26}" ]; then   if [ -z "${NPM_CONFIG_NOPROXY+x}" ] || [ "${NPM_CONFIG_NOPROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_26}" ]; then export NPM_CONFIG_NOPROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_26}"; else export NPM_CONFIG_NOPROXY; fi else builtin unset NPM_CONFIG_NOPROXY 2>/dev/null || command unset NPM_CONFIG_NOPROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_26 __CODEX_SNAPSHOT_PROXY_OVERRIDE_26 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_26 __CODEX_SNAPSHOT_PROXY_OVERRIDE_26 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_27}" ]; then   if [ -z "${NPM_CONFIG_PROXY+x}" ] || [ "${NPM_CONFIG_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_27}" ]; then export NPM_CONFIG_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_27}"; else export NPM_CONFIG_PROXY; fi else builtin unset NPM_CONFIG_PROXY 2>/dev/null || command unset NPM_CONFIG_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_27 __CODEX_SNAPSHOT_PROXY_OVERRIDE_27 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_27 __CODEX_SNAPSHOT_PROXY_OVERRIDE_27 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_28}" ]; then   if [ -z "${PIP_CERT+x}" ] || [ "${PIP_CERT-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_28}" ]; then export PIP_CERT="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_28}"; else export PIP_CERT; fi else builtin unset PIP_CERT 2>/dev/null || command unset PIP_CERT; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_28 __CODEX_SNAPSHOT_PROXY_OVERRIDE_28 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_28 __CODEX_SNAPSHOT_PROXY_OVERRIDE_28 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_29}" ]; then   if [ -z "${PIP_PROXY+x}" ] || [ "${PIP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_29}" ]; then export PIP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_29}"; else export PIP_PROXY; fi else builtin unset PIP_PROXY 2>/dev/null || command unset PIP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_29 __CODEX_SNAPSHOT_PROXY_OVERRIDE_29 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_29 __CODEX_SNAPSHOT_PROXY_OVERRIDE_29 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_30}" ]; then   if [ -z "${REQUESTS_CA_BUNDLE+x}" ] || [ "${REQUESTS_CA_BUNDLE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_30}" ]; then export REQUESTS_CA_BUNDLE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_30}"; else export REQUESTS_CA_BUNDLE; fi else builtin unset REQUESTS_CA_BUNDLE 2>/dev/null || command unset REQUESTS_CA_BUNDLE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_30 __CODEX_SNAPSHOT_PROXY_OVERRIDE_30 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_30 __CODEX_SNAPSHOT_PROXY_OVERRIDE_30 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_31}" ]; then   if [ -z "${SSL_CERT_FILE+x}" ] || [ "${SSL_CERT_FILE-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_31}" ]; then export SSL_CERT_FILE="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_31}"; else export SSL_CERT_FILE; fi else builtin unset SSL_CERT_FILE 2>/dev/null || command unset SSL_CERT_FILE; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_31 __CODEX_SNAPSHOT_PROXY_OVERRIDE_31 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_31 __CODEX_SNAPSHOT_PROXY_OVERRIDE_31 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_32}" ]; then   if [ -z "${WSS_PROXY+x}" ] || [ "${WSS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_32}" ]; then export WSS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_32}"; else export WSS_PROXY; fi else builtin unset WSS_PROXY 2>/dev/null || command unset WSS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_32 __CODEX_SNAPSHOT_PROXY_OVERRIDE_32 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_32 __CODEX_SNAPSHOT_PROXY_OVERRIDE_32 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_33}" ]; then   if [ -z "${WS_PROXY+x}" ] || [ "${WS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_33}" ]; then export WS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_33}"; else export WS_PROXY; fi else builtin unset WS_PROXY 2>/dev/null || command unset WS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_33 __CODEX_SNAPSHOT_PROXY_OVERRIDE_33 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_33 __CODEX_SNAPSHOT_PROXY_OVERRIDE_33 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_34}" ]; then   if [ -z "${YARN_HTTPS_PROXY+x}" ] || [ "${YARN_HTTPS_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_34}" ]; then export YARN_HTTPS_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_34}"; else export YARN_HTTPS_PROXY; fi else builtin unset YARN_HTTPS_PROXY 2>/dev/null || command unset YARN_HTTPS_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_34 __CODEX_SNAPSHOT_PROXY_OVERRIDE_34 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_34 __CODEX_SNAPSHOT_PROXY_OVERRIDE_34 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_35}" ]; then   if [ -z "${YARN_HTTP_PROXY+x}" ] || [ "${YARN_HTTP_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_35}" ]; then export YARN_HTTP_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_35}"; else export YARN_HTTP_PROXY; fi else builtin unset YARN_HTTP_PROXY 2>/dev/null || command unset YARN_HTTP_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_35 __CODEX_SNAPSHOT_PROXY_OVERRIDE_35 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_35 __CODEX_SNAPSHOT_PROXY_OVERRIDE_35 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_36}" ]; then   if [ -z "${YARN_NO_PROXY+x}" ] || [ "${YARN_NO_PROXY-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_36}" ]; then export YARN_NO_PROXY="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_36}"; else export YARN_NO_PROXY; fi else builtin unset YARN_NO_PROXY 2>/dev/null || command unset YARN_NO_PROXY; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_36 __CODEX_SNAPSHOT_PROXY_OVERRIDE_36 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_36 __CODEX_SNAPSHOT_PROXY_OVERRIDE_36 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_37}" ]; then   if [ -z "${all_proxy+x}" ] || [ "${all_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_37}" ]; then export all_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_37}"; else export all_proxy; fi else builtin unset all_proxy 2>/dev/null || command unset all_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_37 __CODEX_SNAPSHOT_PROXY_OVERRIDE_37 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_37 __CODEX_SNAPSHOT_PROXY_OVERRIDE_37 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_38}" ]; then   if [ -z "${ftp_proxy+x}" ] || [ "${ftp_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_38}" ]; then export ftp_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_38}"; else export ftp_proxy; fi else builtin unset ftp_proxy 2>/dev/null || command unset ftp_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_38 __CODEX_SNAPSHOT_PROXY_OVERRIDE_38 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_38 __CODEX_SNAPSHOT_PROXY_OVERRIDE_38 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_39}" ]; then   if [ -z "${http_proxy+x}" ] || [ "${http_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_39}" ]; then export http_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_39}"; else export http_proxy; fi else builtin unset http_proxy 2>/dev/null || command unset http_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_39 __CODEX_SNAPSHOT_PROXY_OVERRIDE_39 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_39 __CODEX_SNAPSHOT_PROXY_OVERRIDE_39 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_40}" ]; then   if [ -z "${https_proxy+x}" ] || [ "${https_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_40}" ]; then export https_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_40}"; else export https_proxy; fi else builtin unset https_proxy 2>/dev/null || command unset https_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_40 __CODEX_SNAPSHOT_PROXY_OVERRIDE_40 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_40 __CODEX_SNAPSHOT_PROXY_OVERRIDE_40 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_41}" ]; then   if [ -z "${no_proxy+x}" ] || [ "${no_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_41}" ]; then export no_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_41}"; else export no_proxy; fi else builtin unset no_proxy 2>/dev/null || command unset no_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_41 __CODEX_SNAPSHOT_PROXY_OVERRIDE_41 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_41 __CODEX_SNAPSHOT_PROXY_OVERRIDE_41 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_42}" ]; then   if [ -z "${npm_config_cafile+x}" ] || [ "${npm_config_cafile-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_42}" ]; then export npm_config_cafile="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_42}"; else export npm_config_cafile; fi else builtin unset npm_config_cafile 2>/dev/null || command unset npm_config_cafile; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_42 __CODEX_SNAPSHOT_PROXY_OVERRIDE_42 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_42 __CODEX_SNAPSHOT_PROXY_OVERRIDE_42 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_43}" ]; then   if [ -z "${npm_config_http_proxy+x}" ] || [ "${npm_config_http_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_43}" ]; then export npm_config_http_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_43}"; else export npm_config_http_proxy; fi else builtin unset npm_config_http_proxy 2>/dev/null || command unset npm_config_http_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_43 __CODEX_SNAPSHOT_PROXY_OVERRIDE_43 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_43 __CODEX_SNAPSHOT_PROXY_OVERRIDE_43 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_44}" ]; then   if [ -z "${npm_config_https_proxy+x}" ] || [ "${npm_config_https_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_44}" ]; then export npm_config_https_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_44}"; else export npm_config_https_proxy; fi else builtin unset npm_config_https_proxy 2>/dev/null || command unset npm_config_https_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_44 __CODEX_SNAPSHOT_PROXY_OVERRIDE_44 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_44 __CODEX_SNAPSHOT_PROXY_OVERRIDE_44 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_45}" ]; then   if [ -z "${npm_config_noproxy+x}" ] || [ "${npm_config_noproxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_45}" ]; then export npm_config_noproxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_45}"; else export npm_config_noproxy; fi else builtin unset npm_config_noproxy 2>/dev/null || command unset npm_config_noproxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_45 __CODEX_SNAPSHOT_PROXY_OVERRIDE_45 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_45 __CODEX_SNAPSHOT_PROXY_OVERRIDE_45 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_46}" ]; then   if [ -z "${npm_config_proxy+x}" ] || [ "${npm_config_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_46}" ]; then export npm_config_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_46}"; else export npm_config_proxy; fi else builtin unset npm_config_proxy 2>/dev/null || command unset npm_config_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_46 __CODEX_SNAPSHOT_PROXY_OVERRIDE_46 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_46 __CODEX_SNAPSHOT_PROXY_OVERRIDE_46 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_47}" ]; then   if [ -z "${ws_proxy+x}" ] || [ "${ws_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_47}" ]; then export ws_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_47}"; else export ws_proxy; fi else builtin unset ws_proxy 2>/dev/null || command unset ws_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_47 __CODEX_SNAPSHOT_PROXY_OVERRIDE_47 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_47 __CODEX_SNAPSHOT_PROXY_OVERRIDE_47 if [ -n "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_48}" ]; then   if [ -z "${wss_proxy+x}" ] || [ "${wss_proxy-}" != "${__CODEX_SNAPSHOT_PROXY_OVERRIDE_48}" ]; then export wss_proxy="${__CODEX_SNAPSHOT_PROXY_OVERRIDE_48}"; else export wss_proxy; fi else builtin unset wss_proxy 2>/dev/null || command unset wss_proxy; fi builtin unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_48 __CODEX_SNAPSHOT_PROXY_OVERRIDE_48 2>/dev/null || command unset __CODEX_SNAPSHOT_PROXY_OVERRIDE_SET_48 __CODEX_SNAPSHOT_PROXY_OVERRIDE_48 fi if [ -n "${PATH:-}" ]; then export PATH='/usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/codex-path':"$PATH"; else export PATH='/usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/codex-path'; fi  exec '/usr/bin/bash' -c 'python3 -c '"'"'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text().splitlines(); matches=[i for i,l in enumerate(s) if "roblox-input-build/machoman run" in l or "roblox-login" in l or "roblox-input" in l and "python3" in l]; print("\n".join("\n".join(s[max(0,i-2):i+4]) for i in matches[-16:]))'"'"' && pgrep -af '"'"'machoman|RobloxPlayer'"'"' '
    ✓ • 0ms

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build -j 4
    [1/42] Generating ../generated/wayland/ext-data-control-v1-server-protocol.h
    [2/42] Generating environment-venv/machoman-venv.json
    [3/42] Installing guest C++ runtimes into dylibs/usr/lib
    [4/42] Installing native x86_64 guest C++ runtimes
    [5/42] Linking CXX shared library dylibs/usr/lib/libobjc.A.dylib.so
    [6/42] Linking CXX executable tests/test_macho_parse
    [7/42] Linking CXX shared library dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation.so
    [8/42] Linking CXX shared library dylibs/usr/lib/libSystem.B.dylib.so
    [9/42] Linking CXX shared library dylibs/System/Library/Frameworks/CoreMIDI.framework/Versions/A/CoreMIDI.so
    [10/42] Linking CXX shared library dylibs/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon.so
    [11/42] Linking CXX shared library dylibs/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate.so
    [12/42] Linking CXX shared library dylibs/System/Library/Frameworks/Metal.framework/Versions/A/Metal.so
    [13/42] Linking CXX shared library dylibs/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo.so
    [14/42] Building C object tests/CMakeFiles/test_wayland_clipboard.dir/__/generated/wayland/ext-data-control-v1-protocol.c.o
    [15/42] Linking CXX shared library dylibs/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText.so
    [16/42] Linking CXX shared library dylibs/usr/lib/machoman-stub-framework.so; Linking stub frameworks
    [17/42] Linking CXX shared library dylibs/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit.so
    [18/42] Linking CXX shared library dylibs/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics.so
    [19/42] Generating guest-venv/machoman-venv.json
    [20/38] Linking CXX executable tests/test_translator
    [21/38] Linking CXX executable tests/test_environment
    [22/38] Linking CXX executable tests/test_runtime_arch
    [23/38] Linking CXX executable tests/test_venv_install
    [24/38] Linking CXX executable tests/test_translation_cache
    [25/38] Linking CXX executable tests/test_native_menu
    [26/38] Linking CXX executable tests/test_commoncrypto
    [27/38] Linking CXX executable tests/test_uuid
    [28/38] Linking CXX executable tests/test_simd_helpers
    [29/38] Linking CXX executable tests/test_dynamic_stubs
    [30/38] Linking CXX executable tests/test_objc_concurrency
    [31/38] Building CXX object tests/CMakeFiles/test_wayland_clipboard.dir/test_wayland_clipboard.cpp.o
    [32/38] Linking CXX executable tests/test_import_audit
    [33/38] Building CXX object tests/CMakeFiles/test_wayland_clipboard.dir/__/src/dylibs/AppKit/clipboard_wayland.cpp.o
    [34/38] Generating environment-venv/bin/environment-arm64
    [35/38] Linking CXX executable tests/test_wayland_clipboard
    [36/38] Generating environment-venv/bin/environment-x86_64
    [37/38] Generating guest-venv/bin/pasteboard-arm64
    /mnt/media/machoman/tests/guest_pasteboard.m:34:59: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       34 |         check(a.changeCount == count && [[a stringForType:NSStringPboardType] isEqualToString:@"Привет 😀\nsecond line"], "legacy string alias and changeCount");
          |                                                           ^~~~~~~~~~~~~~~~~~
          |                                                           NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:35:56: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       35 |         check([[a availableTypeFromArray:@[@"missing", NSStringPboardType]] isEqualToString:NSStringPboardType], "type preference returns requested alias");
          |                                                        ^~~~~~~~~~~~~~~~~~
          |                                                        NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:35:93: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       35 |         check([[a availableTypeFromArray:@[@"missing", NSStringPboardType]] isEqualToString:NSStringPboardType], "type preference returns requested alias");
          |                                                                                             ^~~~~~~~~~~~~~~~~~
          |                                                                                             NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:49:36: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       49 |         check([[item stringForType:NSStringPboardType] isEqualToString:@"first"] && item.types.count == 2, "item data and aliases");
          |                                    ^~~~~~~~~~~~~~~~~~
          |                                    NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:57:34: warning: object of type 'NSObject *' is not compatible with array element type 'id<NSPasteboardWriting>' [-Wobjc-literal-conversion]
       57 |         check(![a writeObjects:@[[NSObject new]]] && a.pasteboardItems.count == 2, "unsupported writer fails without partial write");
          |                                  ^~~~~~~~~~~~~~
    5 warnings generated.
    [38/38] Generating guest-venv/bin/pasteboard-x86_64
    /mnt/media/machoman/tests/guest_pasteboard.m:34:59: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       34 |         check(a.changeCount == count && [[a stringForType:NSStringPboardType] isEqualToString:@"Привет 😀\nsecond line"], "legacy string alias and changeCount");
          |                                                           ^~~~~~~~~~~~~~~~~~
          |                                                           NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:35:56: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       35 |         check([[a availableTypeFromArray:@[@"missing", NSStringPboardType]] isEqualToString:NSStringPboardType], "type preference returns requested alias");
          |                                                        ^~~~~~~~~~~~~~~~~~
          |                                                        NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:35:93: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       35 |         check([[a availableTypeFromArray:@[@"missing", NSStringPboardType]] isEqualToString:NSStringPboardType], "type preference returns requested alias");
          |                                                                                             ^~~~~~~~~~~~~~~~~~
          |                                                                                             NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:49:36: warning: 'NSStringPboardType' is deprecated: first deprecated in macOS 10.14 [-Wdeprecated-declarations]
       49 |         check([[item stringForType:NSStringPboardType] isEqualToString:@"first"] && item.types.count == 2, "item data and aliases");
          |                                    ^~~~~~~~~~~~~~~~~~
          |                                    NSPasteboardTypeString
    /mnt/media/machoman/third_party/sdk/MacOSX26.1.sdk/System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h:504:32: note: 'NSStringPboardType' has been explicitly marked deprecated here
      504 | APPKIT_EXTERN NSPasteboardType NSStringPboardType API_DEPRECATED_WITH_REPLACEMENT("NSPasteboardTypeString", macos(10.0,10.14));
          |                                ^
    /mnt/media/machoman/tests/guest_pasteboard.m:57:34: warning: object of type 'NSObject *' is not compatible with array element type 'id<NSPasteboardWriting>' [-Wobjc-literal-conversion]
       57 |         check(![a writeObjects:@[[NSObject new]]] && a.pasteboardItems.count == 2, "unsupported writer fails without partial write");
          |                                  ^~~~~~~~~~~~~~
    5 warnings generated.
    ✓ • 4.12s

## Activity

    $ python3 -c 'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text().splitlines(); matches=[i for i,l in enumerate(s) if " $ " in l and ("RobloxPlayer" in l or "roblox" in l and ("python3" in l or "env " in l))]; print("\n".join("\n".join(s[max(0,i-2):i+3]) for i in matches[-7:]))' && ls work/*roblox*py work/*text-view*gdb
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Function "ak::(anonymous namespace)::text_selection_changed" not defined.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
    ## Activity
    
        $ gdb -q -batch -x work/text-view-handle-watch.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Breakpoint 1 at 0xaad3d: file /mnt/media/machoman/src/runtime/loader.cpp, line 1884.
        [Thread debugging using libthread_db enabled]
    ## Activity
    
        $ env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
        /usr/bin/bash: line 1:     3 Segmentation fault         (core dumped) env MACHOMAN_TRACE=1 MACHOMAN_APPKIT_HEADLESS=1 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64 > work/logs/text-view-native-trace.log 2>&1
        ✗ (139) • 87ms
    ## Activity
    
        $ gdb -q -batch -x work/text-view-test.gdb --args /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv text-view-x86_64
        Catchpoint 1 (signal SIGSEGV)
        Breakpoint 2 at 0xac133: file /mnt/media/machoman/src/runtime/loader.cpp, line 270.
    work/debug-roblox-cookies.py
    work/roblox-binary-analysis.py
    work/text-view-handle-watch.gdb
    work/text-view-test.gdb
    ✓ • 0ms

## Activity

    $ ctest --test-dir /tmp/machoman-roblox-input-build -R 'guest_(text_view|geometry|focus_reentrancy|string_format)_' --output-on-failure
    Test project /tmp/machoman-roblox-input-build
        Start 52: guest_text_view_arm64
    1/8 Test #52: guest_text_view_arm64 ............   Passed    0.33 sec
        Start 53: guest_focus_reentrancy_arm64
    2/8 Test #53: guest_focus_reentrancy_arm64 .....   Passed    0.30 sec
        Start 55: guest_text_view_x86_64
    3/8 Test #55: guest_text_view_x86_64 ...........   Passed    0.05 sec
        Start 56: guest_focus_reentrancy_x86_64
    4/8 Test #56: guest_focus_reentrancy_x86_64 ....   Passed    0.05 sec
        Start 69: guest_string_format_arm64
    5/8 Test #69: guest_string_format_arm64 ........   Passed    0.26 sec
        Start 70: guest_string_format_x86_64
    6/8 Test #70: guest_string_format_x86_64 .......   Passed    0.01 sec
        Start 71: guest_geometry_arm64
    7/8 Test #71: guest_geometry_arm64 .............   Passed    0.30 sec
        Start 72: guest_geometry_x86_64
    8/8 Test #72: guest_geometry_x86_64 ............   Passed    0.05 sec
    
    100% tests passed out of 8
    
    Total Test time (real) =   1.37 sec
    ✓ • 1.28s

## Activity

    $ sed -n '1,110p' work/roblox-native-fault.gdb && sed -n '1,90p' work/text-view-test.gdb && ls /tmp/machoman-roblox-input-build/tests/CTestTestfile.cmake && rg -n 'MACHOMAN|argparse|--app|--binary|--build' scripts/run-live.py | head -n 55
    set pagination off
    set debuginfod enabled off
    handle SIGSEGV nostop noprint pass
    handle SIGBUS nostop noprint pass
    break on_fault
    condition 1 '(anonymous namespace)::g_guard' == 0
    run
    thread apply all bt 20
    info registers
    p/x '(anonymous namespace)::guest_process::g_current'->ctx
    set pagination off
    set debuginfod enabled off
    set print frame-arguments none
    set breakpoint pending on
    unset environment WAYLAND_DISPLAY
    unset environment DISPLAY
    set environment MACHOMAN_APPKIT_HEADLESS 1
    handle SIGSEGV SIGILL SIGBUS nostop noprint pass
    catch signal SIGSEGV
    commands
    silent
    if '(anonymous namespace)::g_guard' != 0
    continue
    else
    printf "host signal stop\n"
    bt 30
    frame 0
    x/5i $pc
    info registers rax rbx rcx rdx rsi rdi r8 r9
    frame 8
    p library->install_name
    p library->handle
    quit
    end
    end
    break '(anonymous namespace)::on_fault' if g_guard == 0
    commands
    silent
    printf "unguarded fault\n"
    bt 25
    quit
    end
    break 'ak::(anonymous namespace)::text_selection_changed'
    commands
    silent
    printf "selection callback\n"
    continue
    end
    break 'ak::window_object::~window_object'
    commands
    silent
    printf "window destructor\n"
    continue
    end
    break /mnt/media/machoman/src/runtime/loader.cpp:1814
    commands
    silent
    handle SIGSEGV stop noprint pass
    continue
    end
    run
    bt 25
    /tmp/machoman-roblox-input-build/tests/CTestTestfile.cmake
    3:import argparse
    12:parser = argparse.ArgumentParser(description=__doc__)
    15:parser.add_argument("--build-dir", type=Path, help="use an isolated machoman build and its host frameworks")
    52:env.update(MACHOMAN_OSLOG="1", MACHOMAN_APPKIT_TRACE="1", DYLD_PRINT_LIBRARIES="1")
    54:    env.pop("MACHOMAN_APPKIT_TRACE", None)
    56:    env["MACHOMAN_APPKIT_FRAME_TIMING"] = "1"
    58:    env["MACHOMAN_CACHE_TRACE"] = "1"
    62:        env.pop("MACHOMAN_MEVK_TRACE", None)
    64:        env["MACHOMAN_MEVK_TRACE"] = "1"
    66:    env["MACHOMAN_MEVK_PRESENT"] = args.mevk_present
    68:    env["MACHOMAN_STRICT_SELECTORS"] = "1"
    70:    env["MACHOMAN_OBJC_TRACE"] = "1"
    72:    env["MACHOMAN_APPKIT_FORCE_SHOW"] = "1"
    74:    env["MACHOMAN_WATCH"] = args.watch
    75:    env["MACHOMAN_WATCH_LIMIT"] = str(args.watch_limit)
    79:    env["MACHOMAN_WATCH_DUMP"] = args.watch_dump
    80:    env["MACHOMAN_WATCH_DUMP_DIR"] = str(dump_dir)
    82:    env["MACHOMAN_OBJC_BACKTRACE"] = args.backtrace
    84:    env["MACHOMAN_OBJC_INVALID_BACKTRACE"] = "1"
    85:env["MACHOMAN_DIAGNOSTICS"] = "1"
    87:    env["MACHOMAN_DISABLE_GUEST_SUBPROCESSES"] = "1"
    154:                         f"configure with -DMACHOMAN_BUILD_MEVK=ON and run cmake --build")
    ✓ • 0ms

## Activity

    $ python3 -c 'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text().splitlines(); matches=[i for i,l in enumerate(s) if "RobloxPlayer" in l and any(w in l for w in ("Popen", "nohup", "env", "MACHOMAN", "build/machoman")) and not any(w in l for w in ("Core was", "Command Line", "generated by", "Inferior"))]; print("\n".join("\n".join(s[max(0,i-4):i+4]) for i in matches[-8:]))' && ls work/*launch* work/*run*roblox* 2>/dev/null
        ==> work/logs/roblox-proc-http-20261113-041628.log <==
        dyld: /tmp/machoman-roblox-input-build/dylibs/usr/lib/libc++.1.dylib is not a matching-architecture Mach-O dylib
        dyld: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/libmimalloc.3.dylib: missing dependency /usr/lib/libc++.1.dylib
        dyld: /tmp/machoman-roblox-input-build/dylibs/usr/lib/libc++.1.dylib is not a matching-architecture Mach-O dylib
        dyld: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer: missing dependency /usr/lib/libc++.1.dylib
        run: unresolved import __ZNSt9bad_allocC2Ev (from /usr/lib/libc++.1.dylib), called from 0x10000733a (x86_64) __mh_execute_header+0x733a
        run: static initializer 0x100003da8 failed
        run: guest used an import no host dylib provides
        ✓ • 0ms
    
    ## Activity
    
        $ python3 -B work/roblox-binary-analysis.py work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer 0x10001c03c
        0x10001c03c: push rbp
        0x10001c03d: mov rbp, rsp
        0x10001c040: push r15
        ✓ • 284ms
    
    ## Activity
    
        $ python3 -B work/roblox-binary-analysis.py work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer 0x10001bdd4
        0x10001bdd4: push rbp
        0x10001bdd5: mov rbp, rsp
        0x10001bdd8: push rbx
    а еще после ввода пароля он снова крашнулся, кстати мышь до сих пор не работала
    
    ## Activity
    
        $ python3 -B work/roblox-binary-analysis.py work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer 0x10001be6b
        0x10001be6b: push rbp
        0x10001be6c: mov rbp, rsp
        0x10001be6f: push r15
    
        $ rg -n 'NSLog|WKWebView|NSPointInRect' work/roblox-import-audit.tsv
            sed -n '15,25p' src/dylibs/Foundation/strings.cpp
            sed -n '2000,2065p' src/runtime/loader.cpp
        542:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebView    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        543:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebViewConfiguration    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        35502:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSLog    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        35503:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSPointInRect    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        $ rg -n 'NSLog|WKWebView|NSPointInRect' work/roblox-import-audit.tsv
            sed -n '15,25p' src/dylibs/Foundation/strings.cpp
            sed -n '2000,2065p' src/runtime/loader.cpp
        542:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebView    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        543:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebViewConfiguration    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        35502:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSLog    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        35503:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSPointInRect    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        #include <cmath>
            sed -n '15,25p' src/dylibs/Foundation/strings.cpp
            sed -n '2000,2065p' src/runtime/loader.cpp
        542:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebView    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        543:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebViewConfiguration    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        35502:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSLog    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        35503:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSPointInRect    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        #include <cmath>
        #include <cstdio>
            sed -n '2000,2065p' src/runtime/loader.cpp
        542:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebView    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        543:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebViewConfiguration    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
        35502:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSLog    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        35503:host    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _NSPointInRect    lazy    no    /mnt/media/machoman/build/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so
        #include <cmath>
        #include <cstdio>
        #include <cstdlib>
    work/darwin-launchd-core.c
    work/launchd-src-tree.json
    work/launchd-tree.json
    work/live-launch-delegate.disasm
    ✗ (2) • 0ms

## Activity

    $ sed -n '1,190p' scripts/run-live.py && sed -n '1,190p' tests/guest_string_format.m && sed -n '1,110p' src/dylibs/Foundation/geometry.cpp && rg -n 'NSLog|nslog' src/dylibs/Foundation/strings.cpp
    #!/usr/bin/env python3
    """Run the installed Live Trial and capture guest stacks when startup stalls."""
    import argparse
    import ctypes
    import os
    from pathlib import Path
    import resource
    import signal
    import subprocess
    import time
    
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=240)
    parser.add_argument("--keep-running", action="store_true", help="leave Live running without a time deadline; retain the RSS guard")
    parser.add_argument("--build-dir", type=Path, help="use an isolated machoman build and its host frameworks")
    parser.add_argument("--quiet-appkit", action="store_true", help="disable per-event/per-frame AppKit tracing for latency measurements")
    parser.add_argument("--frame-timing", action="store_true", help="report aggregate update/draw/present times every three seconds")
    parser.add_argument("--cache-trace", action="store_true", help="report disk translation cache hits and writes")
    parser.add_argument("--log", type=Path, default=Path("work/live-diagnostic.log"))
    parser.add_argument("--objc", action="store_true", help="trace Objective-C messages")
    parser.add_argument("--force-show", action="store_true", help="show windows for drawing diagnostics")
    parser.add_argument("--mevk", action="store_true", help="prefer the built Metal/MEVK framework and trace GPU submissions")
    parser.add_argument("--mevk-present", choices=("auto", "vulkan", "cpu"), help="select MEVK drawable output; vulkan requires direct Wayland presentation")
    parser.add_argument("--strict-selectors", action="store_true", help="stop at the first missing Objective-C method instead of returning nil")
    parser.add_argument("--no-guest-subprocesses", action="store_true", help="isolate UI diagnostics from memory-heavy Mach-O helper processes")
    parser.add_argument("--gdb", action="store_true", help="attach GDB and save native thread stacks at the first fault without a core dump")
    parser.add_argument("--watch", help="comma-separated guest instruction addresses to trace")
    parser.add_argument("--watch-limit", type=int, default=32, help="maximum log lines per watch address; 0 means unlimited")
    parser.add_argument("--watch-dump", help="VA:register:byte-count[:pointer-offset[:data-offset]] memory snapshots at watched instructions")
    parser.add_argument("--backtrace", help="comma-separated Objective-C selectors to trace with stacks")
    parser.add_argument("--invalid-objc-backtrace", action="store_true", help="print stacks for messages to invalid Objective-C objects")
    parser.add_argument("--dump-after", type=float, help="capture the interrupted PC and guest stacks after this many seconds")
    parser.add_argument("--dump-every", type=float, default=30, help="interval between stack captures with --dump-after")
    parser.add_argument("--rss-limit-gb", type=float, default=5, help="stop if Live RSS exceeds this many GiB; use a cgroup for a hard limit")
    parser.add_argument("--inspect-pid", type=int, help="sample an existing Live process without launching or stopping it")
    parser.add_argument("--stop-pid", type=int, help="terminate an existing machoman Live process after validating its command line")
    parser.add_argument("--inspect-samples", type=int, default=1, help="number of SIGQUIT stack samples of --inspect-pid")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.dump_after is not None and (args.dump_after <= 0 or args.dump_every <= 0):
        parser.error("stack capture times must be positive")
    if args.watch_limit < 0:
        parser.error("--watch-limit must be nonnegative")
    if args.rss_limit_gb <= 0:
        parser.error("--rss-limit-gb must be positive")
    root = Path(__file__).resolve().parent.parent
    build_dir = args.build_dir.resolve() if args.build_dir else root / "build"
    log_path = args.log if args.log.is_absolute() else root / args.log
    log_path.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(MACHOMAN_OSLOG="1", MACHOMAN_APPKIT_TRACE="1", DYLD_PRINT_LIBRARIES="1")
    if args.quiet_appkit:
        env.pop("MACHOMAN_APPKIT_TRACE", None)
    if args.frame_timing:
        env["MACHOMAN_APPKIT_FRAME_TIMING"] = "1"
    if args.cache_trace:
        env["MACHOMAN_CACHE_TRACE"] = "1"
    if args.mevk:
        env["DYLD_FRAMEWORK_PATH"] = str(build_dir / "dylibs/System/Library/Frameworks")
        if args.quiet_appkit:
            env.pop("MACHOMAN_MEVK_TRACE", None)
        else:
            env["MACHOMAN_MEVK_TRACE"] = "1"
    if args.mevk_present:
        env["MACHOMAN_MEVK_PRESENT"] = args.mevk_present
    if args.strict_selectors:
        env["MACHOMAN_STRICT_SELECTORS"] = "1"
    if args.objc:
        env["MACHOMAN_OBJC_TRACE"] = "1"
    if args.force_show:
        env["MACHOMAN_APPKIT_FORCE_SHOW"] = "1"
    if args.watch:
        env["MACHOMAN_WATCH"] = args.watch
        env["MACHOMAN_WATCH_LIMIT"] = str(args.watch_limit)
    if args.watch_dump:
        dump_dir = log_path.with_suffix(".dumps")
        dump_dir.mkdir(parents=True, exist_ok=True)
        env["MACHOMAN_WATCH_DUMP"] = args.watch_dump
        env["MACHOMAN_WATCH_DUMP_DIR"] = str(dump_dir)
    if args.backtrace:
        env["MACHOMAN_OBJC_BACKTRACE"] = args.backtrace
    if args.invalid_objc_backtrace:
        env["MACHOMAN_OBJC_INVALID_BACKTRACE"] = "1"
    env["MACHOMAN_DIAGNOSTICS"] = "1"
    if args.no_guest_subprocesses:
        env["MACHOMAN_DISABLE_GUEST_SUBPROCESSES"] = "1"
    
    def disable_core_dump():
        # Multi-GiB diagnostic cores must not consume RAM-backed /tmp or page cache.
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if args.gdb:
            # GDB is a sibling of Live under this launcher. Allow it to attach
            # with Yama ptrace_scope=1, for this diagnostic child only.
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(ctypes.c_int(0x59616D61), ctypes.c_ulong(-1),
                          ctypes.c_ulong(0), ctypes.c_ulong(0), ctypes.c_ulong(0)) != 0:
                raise OSError(ctypes.get_errno(), "PR_SET_PTRACER for diagnostic GDB")
    
    def memory_usage(pid):
        try:
            fields = {}
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith(("VmRSS:", "VmSize:", "VmSwap:")):
                    name, value, _ = line.split()
                    fields[name.rstrip(":")] = int(value) * 1024
            return fields
        except (FileNotFoundError, ProcessLookupError):
            return {}
    
    if args.inspect_pid is not None or args.stop_pid is not None:
        if args.inspect_pid is not None and args.stop_pid is not None:
            parser.error("choose --inspect-pid or --stop-pid")
        target_pid = args.stop_pid if args.stop_pid is not None else args.inspect_pid
        if args.inspect_samples < 1 or args.inspect_samples > 30:
            parser.error("--inspect-samples must be between 1 and 30")
        try:
            command = Path(f"/proc/{target_pid}/cmdline").read_bytes().split(b"\0")
        except FileNotFoundError:
            parser.exit(1, f"Live PID {target_pid} no longer exists\n")
        if command == [b""]:
            parser.exit(1, f"Live PID {target_pid} has exited (waiting for its launcher to reap it)\n")
        if not command or Path(os.fsdecode(command[0])).name != "machoman" or b"Live" not in command:
            parser.error("PID must refer to the running machoman Live process")
        if args.stop_pid is not None:
            os.kill(target_pid, signal.SIGTERM)
            print(f"Sent SIGTERM to Live PID {target_pid}", flush=True)
            raise SystemExit(0)
        for sample in range(args.inspect_samples):
            usage = memory_usage(args.inspect_pid)
            print(f"Snapshot {sample + 1}: Live PID {args.inspect_pid}; RSS {usage.get('VmRSS', 0) / 2**30:.2f} GiB", flush=True)
            for name in ["wchan", "io"]:
                print(f"{name}: {Path(f'/proc/{args.inspect_pid}/{name}').read_text().strip()}", flush=True)
            proc_root = Path(f"/proc/{args.inspect_pid}")
            if sample == 0:
                print(f"mounts:\n{(proc_root / 'mountinfo').read_text()}", flush=True)
            for fd_path in sorted((proc_root / "fd").iterdir(), key=lambda p: int(p.name)):
                try:
                    target = os.readlink(fd_path)
                    if target.startswith("/") and target not in ("/dev/null", "/dev/urandom", "/dev/random"):
                        info = (proc_root / "fdinfo" / fd_path.name).read_text().strip()
                        print(f"fd {fd_path.name}: {target}\n{info}", flush=True)
                except (FileNotFoundError, ProcessLookupError):
                    pass
            os.kill(args.inspect_pid, signal.SIGQUIT)
            time.sleep(1)
        raise SystemExit(0)
    
    if args.mevk:
        for framework in ("Metal", "CoreGraphics"):
            binary = build_dir / f"dylibs/System/Library/Frameworks/{framework}.framework/Versions/A/{framework}.so"
            if not binary.is_file() or binary.resolve().name == "machoman-stub-framework.so":
                parser.error(f"--mevk requires a built MEVK {framework} framework in {build_dir}; "
                             f"configure with -DMACHOMAN_BUILD_MEVK=ON and run cmake --build")
    
    with log_path.open("wb") as log, log_path.with_suffix(".memory.csv").open("w", buffering=1) as memory_log:
        memory_log.write("seconds,rss_bytes,virtual_bytes,swap_bytes\n")
        process = subprocess.Popen([str(build_dir / "machoman"), "run", str(root / "work/venv"), "Live"],
                                   cwd=root, env=env, stdout=log, stderr=log, start_new_session=True,
                                   preexec_fn=disable_core_dump)
        print(f"Live PID {process.pid}; log: {log_path}", flush=True)
        debugger = None
        debugger_log = None
        if args.gdb:
            debugger_path = log_path.with_suffix(".host-stack.log")
            debugger_log = debugger_path.open("wb")
            debugger = subprocess.Popen([
                "gdb", "-q", "-batch", "-iex", "set debuginfod enabled off",
                "-ex", "set pagination off", "-ex", "handle SIGQUIT nostop noprint pass",
                "-ex", "handle SIGPIPE nostop noprint pass", "-ex", "handle SIGUSR1 nostop noprint pass",
                "-p", str(process.pid), "-ex", "continue",
                "-ex", "thread apply all bt 16", "-ex", "info registers",
                "-ex", "p '(anonymous namespace)::guest_process::g_current'",
                "-ex", "p/x '(anonymous namespace)::guest_process::g_current'->ctx", "-ex", "detach",
            ], stdout=debugger_log, stderr=debugger_log)
            print(f"GDB fault capture: {debugger_path}", flush=True)
        start = time.monotonic()
        dumped = False
        next_dump = args.dump_after
        next_memory_report = 0
        try:
            while process.poll() is None and (args.keep_running or time.monotonic() - start < args.seconds):
                time.sleep(1)
                elapsed = time.monotonic() - start
                usage = memory_usage(process.pid)
                if usage:
                    rss = usage.get("VmRSS", 0)
                    memory_log.write(f"{elapsed:.1f},{rss},{usage.get('VmSize', 0)},{usage.get('VmSwap', 0)}\n")
                    if elapsed >= next_memory_report:
                        print(f"Live memory at {elapsed:.0f}s: RSS {rss / 2**30:.2f} GiB; virtual {usage.get('VmSize', 0) / 2**30:.2f} GiB", flush=True)
    #import <Foundation/Foundation.h>
    #include <stdarg.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    
    static void check(NSString *value, const char *expected) {
        if (!value || strcmp([value UTF8String], expected)) {
            fprintf(stderr, "string formatting: expected '%s', got '%s'\n",
                    expected, value ? [value UTF8String] : "(nil)");
            exit(1);
        }
    }
    static NSString *via_va_list(NSString *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
        va_end(ap);
        return s;
    }
    static CFStringRef cf_via_va_list(CFStringRef fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        CFStringRef s = CFStringCreateWithFormatAndArguments(NULL, NULL, fmt, ap);
        va_end(ap);
        return s;
    }
    int main(void) {
        @autoreleasepool {
            const char *expected = "object 7 2.50 3 4 5 6";
            check([NSString stringWithFormat:@"%@ %d %.2f %d %d %d %d", @"object", 7, 2.5, 3, 4, 5, 6], expected);
            check(via_va_list(@"%@ %d %.2f %d %d %d %d", @"object", 7, 2.5, 3, 4, 5, 6), expected);
            check([[NSString alloc] initWithFormat:@"%@ %d %.1f" locale:nil, @"locale", 11, 3.5], "locale 11 3.5");
            check((NSString *)CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %d %.2f %d %d %d %d"),
                                                      CFSTR("object"), 7, 2.5, 3, 4, 5, 6), expected);
            check((NSString *)cf_via_va_list(CFSTR("%@ %d %.2f %d %d %d %d"),
                                             CFSTR("object"), 7, 2.5, 3, 4, 5, 6), expected);
            NSMutableString *s = [NSMutableString stringWithString:@"start"];
            [s appendFormat:@" %@ %d %.1f", @"objc", 9, 4.5];
            CFStringAppendFormat((CFMutableStringRef)s, NULL, CFSTR(" %@ %d %.1f"), CFSTR("cf"), 13, 6.5);
            check(s, "start objc 9 4.5 cf 13 6.5");
            check([NSString stringWithFormat:@"%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f",
                                             1., 2., 3., 4., 5., 6., 7., 8., 9., 10.], "1 2 3 4 5 6 7 8 9 10");
            check([NSString stringWithFormat:@"%d %.2Lf %d", 17, (long double)1.25, 19], "17 1.25 19");
            char text[80];
            snprintf(text, sizeof text, "%d %.2Lf %d", 17, (long double)1.25, 19);
            if (strcmp(text, "17 1.25 19")) abort();
            NSLog(@"NSLog ABI: %s %@ %d %.1f %d %d %d %d", "C string", @"object", 7, 2.5, 3, 4, 5, 6);
        }
        puts("Darwin NSString and CFString formatting passed");
        return 0;
    }
    #include <algorithm>
    #include <cstdint>
    
    struct ns_rect { double x, y, w, h; };
    extern "C" __attribute__((visibility("default"))) int mm_NSIsEmptyRect(double, double, double, double) __asm__("_NSIsEmptyRect");
    extern "C" int mm_NSIsEmptyRect(double, double, double w, double h) { return w <= 0 || h <= 0; }
    extern "C" __attribute__((visibility("default"))) void mm_NSDivideRect(ns_rect *, ns_rect *, std::uint64_t, double, double, double, double, double) __asm__("_NSDivideRect");
    extern "C" void mm_NSDivideRect(ns_rect *slice, ns_rect *remainder, std::uint64_t edge,
                                    double x, double y, double w, double h, double amount) {
        ns_rect s{x,y,w,h}, r=s;
        amount=std::clamp(amount,0.,std::max(0.,edge==0||edge==2?w:h));
        switch(edge){
        case 0:s.w=amount;r.x+=amount;r.w-=amount;break;
        case 1:s.h=amount;r.y+=amount;r.h-=amount;break;
        case 2:s.x+=w-amount;s.w=amount;r.w-=amount;break;
        case 3:s.y+=h-amount;s.h=amount;r.h-=amount;break;
        default:return;
        }
        if(slice)*slice=s;if(remainder)*remainder=r;
    }
    ✗ (1) • 0ms

## Activity

    $ python3 -c 'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text().splitlines(); matches=[i for i,l in enumerate(s) if "run-live.py" in l and "roblox" in l.lower()]; print("\n".join("\n".join(s[max(0,i-3):i+3]) for i in matches[-5:]))' && cat work/venv/machoman-venv.json | head -n 45
        5280:## Assistant
        5286:    $ gdb -q -batch -iex 'set debuginfod enabled off' -p 158304 -ex 'set pagination off' -ex 'thread apply all bt 12' -ex 'detach' > work/logs/roblox-cookies-01-stacks.log 2>&1
        5291:    $ wc -l work/logs/roblox-cookies-01.log; tail -5 work/logs/roblox-cookies-01-stacks.log; sed -n '1190,1260p' work/logs/roblox-cookies-01.log
        5371:    $ rg -n 'cookie|Unauthorized|lms|userDidLogin|clear|Guest|auth' '/home/monsler/Library/Application Support/Logs/Roblox/0.747.0.7471481_20261113T003308Z_Player_6d6d4_last.log' | tail -35; tail -10 work/logs/roblox-cookies-01.log; rg -n 'native.*backtrace|print_guest_backtrace|sigusr|SIGUSR' src/runtime/loader.cpp src/dylibs/libSystem/libSystem.cpp scripts/run-live.py | head -40
        5474:    $ env MACHOMAN_COOKIE_TRACE=1 MACHOMAN_OSLOG=1 timeout -k 3s 150s ./build/machoman run work/venv RobloxPlayer > work/logs/roblox-cookies-01.log 2>&1
        5641:## Assistant
        5676:    $ python3 -B work/debug-roblox-cookies.py
        5683:    $ tail -25 work/logs/roblox-cookies-debug.log; sed -n '1,95p' work/roblox-studio-lms-recipe.disasm; sed -n '1,85p' work/roblox-studio-lms-report.disasm; tail -3 work/logs/roblox-cookies-01.log; wc -l work/logs/roblox-cookies-debug.log
        5896:    $ sed -n '1,84p' work/logs/roblox-cookies-debug.log
        5985:    $ rg -n 'PR_SET_PTRACER|prctl|ptracer|ptrace' src/runtime/loader.cpp scripts/run-live.py; sed -n '260,335p' src/runtime/loader.cpp; sed -n '60,90p' src/runtime/loader.cpp; sed -n '1560,1660p' work/logs/roblox-cookies-01.log
        6165:## User
        6169:## Assistant
    {
      "machoman_venv_version": 1,
      "target_arch": "x86_64",
      "source_arch": "arm64",
      "dylibs": []
    }
    ✓ • 0ms

## Activity

    $ tail -n 30 work/logs/resumed-roblox-input-tests.log && ls -lt work/logs/roblox* | head -n 18
            Start 114: guest_kvo_smoke
    111/126 Test #107: guest_fork_exec_x86_64_arm64 .........   Passed    0.28 sec
            Start 115: guest_cxx_smoke
    112/126 Test #112: guest_resource_stubs .................   Passed    0.27 sec
            Start 116: guest_audio_null
    113/126 Test #114: guest_kvo_smoke ......................   Passed    0.32 sec
            Start 117: native_dialog
    114/126 Test #116: guest_audio_null .....................   Passed    0.27 sec
            Start 118: nib_archive
    115/126 Test #118: nib_archive ..........................   Passed    0.00 sec
            Start 119: guest_dialog_smoke
    116/126 Test #119: guest_dialog_smoke ...................   Passed    0.38 sec
            Start 120: cocoa_layout
    117/126 Test #120: cocoa_layout .........................   Passed    0.00 sec
            Start 121: guest_scanning_smoke
    118/126 Test #113: guest_runtime_smoke ..................   Passed    0.98 sec
            Start 122: guest_bitmap_drawing
    119/126 Test #121: guest_scanning_smoke .................   Passed    0.34 sec
            Start 123: guest_rpath_loader
    120/126 Test #115: guest_cxx_smoke ......................   Passed    1.16 sec
            Start 124: guest_rpath_executable
    121/126 Test #122: guest_bitmap_drawing .................   Passed    0.31 sec
            Start 125: ipc_kernel
    122/126 Test #125: ipc_kernel ...........................   Passed    0.01 sec
            Start 126: ipc_transport
    123/126 Test #126: ipc_transport ........................Subprocess aborted***Exception:   0.10 sec
    IPC transport: immutable context starts broker without environment variables (errno 1)
    
    124/126 Test #123: guest_rpath_loader ...................   Passed    0.28 sec
    125/126 Test #124: guest_rpath_executable ...............   Passed    0.28 sec
    -rwxrwxrwx 1 nobody nobody    151101 Nov 13 04:40 work/logs/roblox-proc-http-20261113-043847.log
    -rwxrwxrwx 1 nobody nobody    163212 Nov 13 04:25 work/logs/roblox-proc-http-20261113-041943.log
    -rwxrwxrwx 1 nobody nobody      2040 Nov 13 04:21 work/logs/roblox-text-view-ui-state.log
    -rwxrwxrwx 1 nobody nobody       737 Nov 13 04:16 work/logs/roblox-proc-http-20261113-041628.log
    -rwxrwxrwx 1 nobody nobody     13970 Nov 13 04:13 work/logs/roblox-input-isolated-build.log
    -rwxrwxrwx 1 nobody nobody     33308 Nov 13 04:06 work/logs/roblox-login-input-core.log
    -rwxrwxrwx 1 nobody nobody   1158154 Nov 13 04:04 work/logs/roblox-proc-http-20261113-035921.log
    -rwxrwxrwx 1 nobody nobody     35910 Nov 13 04:03 work/logs/roblox-login-input-stacks.log
    -rwxrwxrwx 1 nobody nobody       873 Nov 13 04:01 work/logs/roblox-login-input-before-1-949x1028.json
    -rwxrwxrwx 1 nobody nobody    659833 Nov 13 04:01 work/logs/roblox-login-input-before-1-949x1028.png
    -rwxrwxrwx 1 nobody nobody    138420 Nov 13 03:58 work/logs/roblox-proc-http-20261113-035720.log
    -rwxrwxrwx 1 nobody nobody      1454 Nov 13 03:58 work/logs/roblox-proc-http-20261113-035720-stacks-0.log
    -rwxrwxrwx 1 nobody nobody       875 Nov 13 03:58 work/logs/roblox-proc-http-20261113-035720-frame-0-1-949x1028.json
    -rwxrwxrwx 1 nobody nobody      8325 Nov 13 03:58 work/logs/roblox-proc-http-20261113-035720-frame-0-1-949x1028.png
    -rwxrwxrwx 1 nobody nobody    110079 Nov 13 03:56 work/logs/roblox-notifications-01.log
    -rwxrwxrwx 1 nobody nobody    105805 Nov 13 03:56 work/logs/roblox-notifications-01-stacks-1.log
    -rwxrwxrwx 1 nobody nobody       873 Nov 13 03:56 work/logs/roblox-notifications-01-frame-1-1-949x1028.json
    -rwxrwxrwx 1 nobody nobody      8890 Nov 13 03:56 work/logs/roblox-notifications-01-frame-1-1-949x1028.png
    ls: write error: Broken pipe
    ✓ • 379ms

## Activity

    $ python3 -c 'from pathlib import Path; s=Path("codex-session-01a1d264-7321-70b3-aa93-5b1ff0aa9e05.md").read_text().splitlines(); matches=[i for i,l in enumerate(s) if any(w in l for w in ("Popen(", "roblox-input-", "roblox-input-build")) and i>18000 and not any(w in l for w in ("[", "#", "Core", "[LWP", "machoman run", "cmake --build"))]; print("\n".join("\n".join(s[max(0,i-3):i+6]) for i in matches[:18]))' && sed -n '245,310p' src/cli/main.cpp && rg -n 'framework|dylibs|build_root|exe.*path' src/runtime/loader.cpp | head -n 45
    
    ## Activity
    
        $ coredumpctl --no-pager debug 193152 --debugger-arguments='-q -batch -x work/roblox-input-metadata-core.gdb'
                   PID: 193152 (machoman)
                   TID: 193152 (machoman)
                   UID: 1000 (monsler)
                   GID: 1000 (monsler)
                Signal: 11 (SEGV) si_code: SI_TKILL
                Signal: 11 (SEGV) si_code: SI_TKILL
             Timestamp: Fri 2026-11-13 04:25:15 MSK (9min ago)
          Command Line: /tmp/machoman-roblox-input-build/machoman run /mnt/media/machoman/work/venv RobloxPlayer
            Executable: /tmp/machoman-roblox-input-build/machoman
         Control Group: /user.slice/user-1000.slice/user@1000.service/app.slice/app-com.microsoft.VSCode-134805.scope
                  Unit: user@1000.service
             User Unit: app-com.microsoft.VSCode-134805.scope
                 Slice: user-1000.slice
             Owner UID: 1000 (monsler)
        202        if (previous && previous != window && w(previous)->first_responder == view) {
        $1 = (mmfw::id) 0x5645e88810b0
        $2 = (mmfw::id) 0x0
        work/roblox-input-metadata-core.gdb:8: Error in sourced command file:
        No symbol "class_rw" in current context.
        ✗ (1) • 5.07s
    
    ## Activity
    
    ## Activity
    
        file changes: Completed · 1 changes
        Update { move_path: None }: /mnt/media/machoman/work/roblox-input-metadata-core.gdb
        @@ -7,4 +7,3 @@
         set $vclass = ((ak::view_object *)view)->base.isa
        -p ((class_rw *)($vclass->bits & ~7))->name
        -p ((class_rw *)($vclass->bits & ~7))->instance_size
        +p ((' (anonymous namespace)::class_rw' *)($vclass->bits & ~7))->name
    ## Activity
    
        file changes: Completed · 1 changes
        Update { move_path: None }: /mnt/media/machoman/work/roblox-input-metadata-core.gdb
        @@ -7,3 +7,4 @@
         set $vclass = ((ak::view_object *)view)->base.isa
        -p ((' (anonymous namespace)::class_rw' *)($vclass->bits & ~7))->name
        +p (('(anonymous namespace)::class_rw' *)($vclass->bits & ~7))->name
        +p (('(anonymous namespace)::class_rw' *)($vclass->bits & ~7))->instance_size
        root = Path('/mnt/media/machoman')
        env = os.environ.copy()
        env.update(MACHOMAN_COOKIE_TRACE='1', MACHOMAN_OSLOG='1')
        build_dir = Path(env.get('ROBLOX_BUILD_DIR', '/tmp/machoman-roblox-input-build'))
        env['MACHOMAN_DYLIB_PATH'] = str(build_dir / 'dylibs/x86_64') + ':' + str(build_dir / 'dylibs')
        prefix = os.environ.get('ROBLOX_DIAGNOSTIC_PREFIX', 'roblox-proc-http-' + time.strftime('%Y%m%d-%H%M%S'))
        interactive = '--interactive' in sys.argv
        settings_path = root / 'work/venv/Applications/RobloxPlayer.app/Contents/MacOS/ClientSettings/ClientAppSettings.json'
        original_settings = settings_path.read_bytes()
    
    ## Activity
    
        $ coredumpctl --no-pager debug 193152 --debugger-arguments='-q -batch -x work/roblox-input-metadata-core.gdb' | tail -n 25
    
        warning: Can't open file /dev/shm/0928ae41-8a19-4ab4-b0d1-4adadeb36ee6 (deleted) during file-backed mapping note processing
        #0  0x00007fbab649a11c in ?? () from /usr/lib/libc.so.6
        [Current thread is 1 (Thread 0x7fbab6b40840 (LWP 193152))]
        #4  0x00007fbab60494d5 in ak::(anonymous namespace)::set_window_recursively (view=..., window=...) at /mnt/media/machoman/src/dylibs/AppKit/views.cpp:202
        [0/1] Re-running CMake...
        -- Configuring done (0.3s)
        -- Generating done (0.1s)
        -- Build files have been written to: /tmp/machoman-roblox-input-build
        [1/122] Generating generated/wayland/ext-data-control-v1-client-protocol.h
        [2/122] Generating generated/wayland/ext-data-control-v1-protocol.c
        [3/122] Building CXX object tests/CMakeFiles/rpath_late_host.dir/rpath_late_host.cpp.o
        [4/122] Building CXX object tests/CMakeFiles/bitmap_drawing_probe.dir/bitmap_drawing_probe.cpp.o
        [5/122] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/appkit_main.cpp.o
        [36/122] Building CXX object tests/CMakeFiles/test_native_menu.dir/test_native_menu.cpp.o
        [37/122] Linking CXX shared library dylibs/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics.so
        [38/122] Generating guest-venv/machoman-venv.json
        created venv at /tmp/machoman-roblox-input-build/modules/MEVK/src/metal/tests/guest-venv
          bin/
          original/
          usr/lib/
          System/Library/Frameworks/
        [39/122] Linking CXX shared library tests/guest-venv/usr/lib/libaudio_null_probe.dylib.so
        [39/122] Linking CXX shared library tests/guest-venv/usr/lib/libaudio_null_probe.dylib.so
        [40/122] Linking CXX shared library tests/guest-venv/usr/lib/libbitmap_drawing_probe.dylib.so
        [41/122] Generating environment-venv/machoman-venv.json
        created venv at /tmp/machoman-roblox-input-build/tests/environment-venv
          bin/
          original/
          usr/lib/
          System/Library/Frameworks/
        [42/122] Linking CXX shared library tests/guest-venv/rpath-fixture/liblocal.dylib.so
         env = os.environ.copy()
        -env.update(MACHOMAN_COOKIE_TRACE='1', MACHOMAN_OSLOG='1')
        +env.update(MACHOMAN_COOKIE_TRACE='1', MACHOMAN_OSLOG='1', MACHOMAN_APPKIT_POINTER_TRACE='1')
         build_dir = Path(env.get('ROBLOX_BUILD_DIR', '/tmp/machoman-roblox-input-build'))
    
    ## Activity
    
        $ cmake --build /tmp/machoman-roblox-input-build --target AppKit guest_text_view_arm64_binary guest_text_view_x86_64_binary -j 4
        [0/1] Re-running CMake...
        [0/1] Re-running CMake...
        -- Configuring done (0.3s)
        -- Generating done (0.1s)
        -- Build files have been written to: /tmp/machoman-roblox-input-build
        [1/6] Generating guest-venv/bin/text-view-arm64
        [2/6] Generating guest-venv/bin/text-view-x86_64
        [3/6] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/views.cpp.o
        /mnt/media/machoman/src/dylibs/AppKit/views.cpp:925:47: warning: unused parameter 'self' [-Wunused-parameter]
          925 | mm_two_doubles window_convertRectToBacking(id self, SEL, double x, double y, double width, double height) {
    
    ## Activity
    
        $ ctest --test-dir /tmp/machoman-roblox-input-build -R 'guest_(text_view|native_geometry|focus_reentrancy|pasteboard)_' --output-on-failure
        Test project /tmp/machoman-roblox-input-build
            Start 51: guest_pasteboard_arm64
        1/6 Test #51: guest_pasteboard_arm64 ...........***Failed  Required regular expression not found. Regex=[NSPasteboard typed data, named boards, writers and clipboard editing passed
        ]  0.32 sec
        pasteboard: property list round trip
    ## Activity
    
        $ ctest --test-dir /tmp/machoman-roblox-input-build -R 'guest_(text_view|native_geometry|focus_reentrancy|pasteboard)_' --output-on-failure
        Test project /tmp/machoman-roblox-input-build
            Start 51: guest_pasteboard_arm64
        1/6 Test #51: guest_pasteboard_arm64 ...........***Failed  Required regular expression not found. Regex=[NSPasteboard typed data, named boards, writers and clipboard editing passed
        ]  0.32 sec
        pasteboard: property list round trip
    
    ## Activity
    
        $ rg -n 'fault|host-origin|on_fault|t_thread' src/runtime/loader.cpp | head -n 30
            cat /tmp/machoman-roblox-input-build/tests/Testing/Temporary/LastTest.log | tail -n 60
        57: * thread, including a fault lookup that needs no global code-map mutex. */
        58:thread_local std::span<const mm_runtime_code *const> g_callback_fault_codes;
        212: * it fault in a recognisable way, so the runtime can name the import. One
        252:/* Guest memory faults land here. The translated code never holds host
        255:struct fault_guard {
        346:            sigaction(fault_signals[i], &sa, &g_previous_fault[i]);
        348:    ~fault_handler_scope() {
        349:        if (--g_fault_scope_depth != 0) return;
        cat: /tmp/machoman-roblox-input-build/tests/Testing/Temporary/LastTest.log: No such file or directory
        ✓ • 0ms
    
    ## Activity
    
        $ rg -n 'view_object::~|window_object::~' src/dylibs/AppKit/views.cpp
        terminate called without an active exception
    
        ==> work/logs/roblox-proc-http-20261113-041628.log <==
        dyld: /tmp/machoman-roblox-input-build/dylibs/usr/lib/libc++.1.dylib is not a matching-architecture Mach-O dylib
        dyld: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/libmimalloc.3.dylib: missing dependency /usr/lib/libc++.1.dylib
        dyld: /tmp/machoman-roblox-input-build/dylibs/usr/lib/libc++.1.dylib is not a matching-architecture Mach-O dylib
        dyld: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer: missing dependency /usr/lib/libc++.1.dylib
        run: unresolved import __ZNSt9bad_allocC2Ev (from /usr/lib/libc++.1.dylib), called from 0x10000733a (x86_64) __mh_execute_header+0x733a
        run: static initializer 0x100003da8 failed
        ==> work/logs/roblox-proc-http-20261113-041628.log <==
        dyld: /tmp/machoman-roblox-input-build/dylibs/usr/lib/libc++.1.dylib is not a matching-architecture Mach-O dylib
        dyld: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/libmimalloc.3.dylib: missing dependency /usr/lib/libc++.1.dylib
        dyld: /tmp/machoman-roblox-input-build/dylibs/usr/lib/libc++.1.dylib is not a matching-architecture Mach-O dylib
        dyld: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer: missing dependency /usr/lib/libc++.1.dylib
        run: unresolved import __ZNSt9bad_allocC2Ev (from /usr/lib/libc++.1.dylib), called from 0x10000733a (x86_64) __mh_execute_header+0x733a
        run: static initializer 0x100003da8 failed
        run: guest used an import no host dylib provides
    
                return std::string(it->name);
            return std::format("{}+{:#x}", it->name, va - it->address);
        }
    };
    
    int cmd_disasm(const char *path, const char *va_str, const char *count_str) {
        std::vector<std::uint8_t> data;
        mm_macho_image img;
        if (!load_image(path, data, img) || !require_arm64(path, img))
            return 1;
        const auto code = img.executable_code();
        if (code.bytes.empty()) {
            std::println(stderr, "error: no executable sections found");
            return 1;
        }
        const std::uint64_t va = std::strtoull(va_str, nullptr, 0);
        const std::size_t count = count_str ? std::strtoul(count_str, nullptr, 0) : 16;
        if (va < code.addr || va >= code.addr + code.bytes.size()) {
            std::println(stderr, "error: {:#x} is not inside __TEXT code [{:#x}, {:#x})", va, code.addr, code.addr + code.bytes.size());
            return 1;
        }
        const disasm_symbols symbols(img);
        std::println("; {}", symbols.describe(va));
        for (std::size_t i = 0; i < count; i++) {
            const std::uint64_t pc = va + i * 4;
            const std::uint64_t off = pc - code.addr;
            if (off + 4 > code.bytes.size())
                break;
            const mm_a64_insn insn = mm_a64_decode(code.bytes.subspan(off, 4), pc);
            std::string note;
            if (insn.op == mm_a64_opcode::unknown)
                note = std::string("  [") + mm_a64_class_name(insn.raw) + "]";
            else if (insn.op == mm_a64_opcode::bl || insn.op == mm_a64_opcode::b || insn.op == mm_a64_opcode::adrp)
                note = "  ; " + symbols.describe(static_cast<std::uint64_t>(insn.imm));
            std::println("{:#x}  {:08x}  {:<8} rd={:<2} rn={:<2} rm={:<2} imm={:#x}{}", pc, insn.raw,
                         mm_a64_opcode_name(insn.op), insn.rd, insn.rn, insn.rm,
                         static_cast<std::uint64_t>(insn.imm), note);
        }
        return 0;
    }
    
    int cmd_run(const char *venv_dir, const char *binary, std::span<char *const> args, std::span<char *const> env,
                bool apply_defaults, std::optional<std::uint32_t> signal_mask) {
        mm_venv venv;
        if (auto st = mm_venv_open(venv_dir, venv); st != mm_venv_status::ok) {
            std::println(stderr, "error: {}: {}", mm_venv_status_str(st), venv.manifest_error);
            return 1;
        }
    
        const mm_launch_request req{.venv = &venv, .executable_path = binary, .argv = args, .env = env,
                                    .apply_venv_env = apply_defaults, .initial_signal_mask = signal_mask};
        const mm_runtime_status st = mm_runtime_launch(req);
        std::println(stderr, "run: {}", mm_runtime_status_str(st));
        return st == mm_runtime_status::ok ? 0 : 1;
    }
    
    int cmd_audit(const char *venv_dir, const char *binary, std::span<char *const> env) {
        mm_venv venv;
        if (auto st = mm_venv_open(venv_dir, venv); st != mm_venv_status::ok) {
            std::println(stderr, "error: {}: {}", mm_venv_status_str(st), venv.manifest_error); return 1;
        }
        mm_import_audit report;
        const mm_launch_request request{.venv = &venv, .executable_path = binary, .env = env};
        if (auto st = mm_runtime_audit(request, report); st != mm_runtime_status::ok) {
            std::println(stderr, "audit: {}", mm_runtime_status_str(st)); return 1;
        }
    110:/* The part of an install name a framework search path is joined to:
    111: * "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit" is looked
    112: * up as "AppKit.framework/Versions/C/AppKit" under each DYLD_FRAMEWORK_PATH
    113: * entry. Empty when the name is not a framework. */
    114:std::string framework_partial_path(std::string_view install_name) {
    115:    const auto at = install_name.find(".framework/");
    674:    guest_image *load_executable(const std::filesystem::path &path, std::vector<std::uint8_t> file, const mm_macho_image &img) {
    756:     * since umbrella frameworks re-export what other libraries define. */
    787:             * statically (a framework stub deriving constants and stub
    806:         * framework stub deriving a constant or a reporting function from
    809:         * framework re-exports what its members define, so CoreServices
    845:        if (path_token(p,"@executable_path"))
    846:            return {exe_dir_/suffix("@executable_path"),fs::path(std::string(MM_VENV_DIR_BIN))/suffix("@executable_path")};
    875:        const std::string framework = framework_partial_path(install_name);
    878:                const bool frameworks = std::string_view(variable).find("FRAMEWORK") != std::string_view::npos;
    879:                if (frameworks && framework.empty()) continue;
    880:                const std::string rest = frameworks ? framework : leaf;
    907:        /* Development convenience: the dylibs built next to the machoman binary. */
    910:                roots.push_back(self.parent_path() / "dylibs/x86_64");
    911:            roots.push_back(self.parent_path() / "dylibs");
    1000:        for (const auto &d : g.img.dylibs) {
    1067:    std::uint64_t next_base_ = 0x200000000ull;           /* dylibs are placed from here upwards */
    1225: * process; host dylibs reach it through the mm_host_api callbacks. */
    1228:    guest_process(const mm_venv &venv, std::span<char *const> args, const std::filesystem::path &exe_path,
    1230:        : exe_path_(exe_path), environment_(environment), venv_root_(std::filesystem::absolute(venv.root).string()),
    1231:          launcher_(std::move(launcher)), executable_path_(exe_path.string()),
    1232:          linker_(venv, std::filesystem::weakly_canonical(exe_path).parent_path(), environment) {
    1234:            arg_storage_.push_back(exe_path.string());
    1239:        /* dyld's "apple" array; its first entry is the executable path. */
    1240:        apple_storage_.push_back("executable_path=" + exe_path.string());
    1250:        // thread contexts and host dylibs are still available.
    1325:    std::filesystem::path exe_path_;
    1327:    std::string venv_root_, launcher_, executable_path_;
    1385:    main_ = linker_.load_executable(exe_path_, std::move(file), img);
    1484:                       &api_environment_put, &api_bind_environment, venv_root_.c_str(), launcher_.c_str(), executable_path_.c_str()};
    1509:     * report the same initializer (every stub framework sees libobjc's);
    1576: * servicing calls into host dylibs on the way. */
    1692:            const auto &dylibs = imp.image->img.dylibs;
    1694:                         imp.bind.dylib_ordinal >= 1 && static_cast<std::size_t>(imp.bind.dylib_ordinal) <= dylibs.size()
    1695:                             ? std::string(dylibs[static_cast<std::size_t>(imp.bind.dylib_ordinal) - 1].name).c_str() : "?",
    1906:     * initialized frameworks, and the same guest identity as main. */
    2497: * libc and framework workers read them. Only runtime configuration is overlaid;
    2541:    const auto path = mm_venv_resolve_executable(*request.venv, request.executable_path, error);
    2552:    if (!ld.load_executable(path, std::move(data), image)) return mm_runtime_status::map_failed;
    2556:        for (std::size_t i = 0; i < loaded->img.dylibs.size(); ++i) {
    ✓ • 448ms

## Activity

    $ ctest --test-dir /tmp/machoman-roblox-input-build --output-on-failure -j 4 > work/logs/resumed-roblox-input-tests.log 2>&1
    ✗ (8) • 21.99s

## Assistant

Последняя правка прошла проверку: Intel-тест больше не падает при завершении. Все восемь проверок ввода, фокуса, координат и форматирования проходят на arm64 и x86_64. Запускаю Roblox, чтобы проверить поведение в самом приложении.

## Activity

    $ pgrep -af '^(/tmp/[^ ]*/machoman|/mnt/media/machoman/build/machoman|./build/machoman)'
    ✗ (1) • 2ms

## Activity

    $ tail -n 22 work/logs/resumed-roblox-input-tests.log
    The following tests FAILED:
          1 - mevk_buffers (Failed)
          2 - mevk_resource_reuse (Subprocess aborted)
          5 - mevk_air_resources (Failed)
          6 - mevk_render (Failed)
          7 - evilcocoa_renderer (Failed)
          9 - mevk_guest_metal (Failed)
         12 - mevk_guest_render (Failed)
         21 - wayland_clipboard (Failed)
         45 - guest_opengl_threads_arm64 (Failed)
         46 - guest_opengl_threads_x86_64 (Failed)
         94 - guest_mach_ports_arm64 (Subprocess aborted)
         95 - guest_mach_ports_x86_64 (Subprocess aborted)
         96 - guest_mach_notifications_arm64 (Failed)
         97 - guest_mach_exchange_arm64_arm64 (Failed)
         98 - guest_mach_exchange_arm64_x86_64 (Failed)
         99 - guest_mach_notifications_x86_64 (Failed)
        100 - guest_mach_exchange_x86_64_arm64 (Failed)
        101 - guest_mach_exchange_x86_64_x86_64 (Failed)
        117 - native_dialog (Timeout)
        126 - ipc_transport (Subprocess aborted)
    Errors while running CTest
    ✓ • 747ms

## Activity

    $ rg -l 'ROBLOX_DIAGNOSTIC_PREFIX|ROBLOX_BUILD_DIR' work -g '*.py' -g '!venv/**' -g '!base-venv/**' -g '!bootstrap-shell-downloads/**'
    work/debug-roblox-cookies.py
    ✓ • 2.72s

## Activity

    $ sed -n '1,230p' work/debug-roblox-cookies.py
    """Launch Roblox under its debugger; capture startup and stopped thread stacks."""
    import os
    import json
    import ctypes
    from pathlib import Path
    import signal
    import subprocess
    import sys
    import time
    
    root = Path('/mnt/media/machoman')
    env = os.environ.copy()
    env.update(MACHOMAN_COOKIE_TRACE='1', MACHOMAN_OSLOG='1', MACHOMAN_APPKIT_POINTER_TRACE='1')
    build_dir = Path(env.get('ROBLOX_BUILD_DIR', '/tmp/machoman-roblox-input-build'))
    env['MACHOMAN_DYLIB_PATH'] = str(build_dir / 'dylibs/x86_64') + ':' + str(build_dir / 'dylibs')
    prefix = os.environ.get('ROBLOX_DIAGNOSTIC_PREFIX', 'roblox-proc-http-' + time.strftime('%Y%m%d-%H%M%S'))
    interactive = '--interactive' in sys.argv
    settings_path = root / 'work/venv/Applications/RobloxPlayer.app/Contents/MacOS/ClientSettings/ClientAppSettings.json'
    original_settings = settings_path.read_bytes()
    settings = json.loads(original_settings)
    # Light trace records request URLs and status, without logging cookie/header values.
    settings.update(DFLogHttpTraceLight=6, FLogScriptContext=6, FLogScriptContextAdd=6,
        FLogContentProviderRequests=6, FLogLoginController=6, FLogLoginUIManager=6)
    
    def enable_debugger():
        # Same diagnostic-child setup as scripts/run-live.py. This avoids slowing
        # startup by making gdb intercept every native guest host-call trap.
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(ctypes.c_int(0x59616D61), ctypes.c_ulong(-1), 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), 'PR_SET_PTRACER for diagnostic child')
    
    with (root / f'work/logs/{prefix}.log').open('w') as log:
        settings_path.write_text(json.dumps(settings, indent=2) + '\n')
        app = subprocess.Popen([str(build_dir / 'machoman'), 'run', str(root / 'work/venv'), 'RobloxPlayer'],
            cwd=root, env=env, stdout=log, stderr=log, start_new_session=True, preexec_fn=enable_debugger)
        try:
            print(f'Roblox PID {app.pid}', flush=True)
            if interactive:
                print('Interactive login run: no timeout or automatic screenshots', flush=True)
                app.wait()
            for sample in range(0 if interactive else 3):
                time.sleep(45)
                if app.poll() is not None: break
                subprocess.run(['python3', '-B', 'work/capture-live-windows.py', '--pid', str(app.pid),
                    '--prefix', f'work/logs/{prefix}-frame-{sample}', '--seconds', '3'], timeout=15, check=False)
                with (root / f'work/logs/{prefix}-stacks-{sample}.log').open('w') as stacks:
                    subprocess.run(['gdb', '-q', '-batch', '-iex', 'set debuginfod enabled off', '-p', str(app.pid),
                        '-x', 'work/roblox-guest-stacks.gdb'], stdout=stacks, stderr=stacks, timeout=15, check=False)
        finally:
            settings_path.write_bytes(original_settings)
            if app.poll() is None:
                app.terminate()
                try: app.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(app.pid, signal.SIGKILL)
                    app.wait()
        print(f'Saved work/logs/{prefix} diagnostics', flush=True)
    ✓ • 0ms

## Activity

    $ ctest --test-dir /tmp/machoman-roblox-input-build --output-on-failure -j 4 > work/logs/resumed-roblox-input-tests-host.log 2>&1
    ✗ (8) • 7.23s

## Activity

    $ tail -n 24 work/logs/resumed-roblox-input-tests-host.log && sed -n '1,160p' work/capture-live-windows.py
    117/126 Test  #32: commoncrypto .........................   Passed    0.00 sec
            Start  58: import_audit_x86_64
    118/126 Test  #70: guest_string_format_x86_64 ...........   Passed    0.01 sec
            Start 118: nib_archive
    119/126 Test  #14: environment ..........................   Passed    0.00 sec
            Start  33: uuid
    120/126 Test #125: ipc_kernel ...........................   Passed    0.01 sec
            Start  24: audio_engine
    121/126 Test #118: nib_archive ..........................   Passed    0.00 sec
            Start 120: cocoa_layout
    122/126 Test  #33: uuid .................................   Passed    0.00 sec
            Start  35: objc_concurrency
    123/126 Test  #58: import_audit_x86_64 ..................   Passed    0.00 sec
    124/126 Test  #24: audio_engine .........................   Passed    0.00 sec
    125/126 Test #120: cocoa_layout .........................   Passed    0.00 sec
    126/126 Test  #35: objc_concurrency .....................   Passed    0.00 sec
    
    99% tests passed, 1 tests failed out of 126
    
    Total Test time (real) =   7.35 sec
    
    The following tests FAILED:
          7 - evilcocoa_renderer (Failed)
    Errors while running CTest
    """Capture only the specified Live process's visible windows for diagnostics."""
    import argparse
    import json
    from pathlib import Path
    import subprocess
    import time
    
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=45)
    args = parser.parse_args()
    seen = set()
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        monitors = json.loads(subprocess.check_output(['hyprctl', 'monitors', '-j']))
        visible_workspaces = {monitor['activeWorkspace']['id'] for monitor in monitors}
        clients = json.loads(subprocess.check_output(['hyprctl', 'clients', '-j']))
        for client in clients:
            if client['pid'] != args.pid or not client['mapped'] or client.get('hidden'):
                continue
            if client['workspace']['id'] not in visible_workspaces:
                continue
            width, height = client['size']
            key = (client['address'], width, height)
            if key in seen:
                continue
            # Allow the first drawable to reach the compositor.
            time.sleep(0.6)
            current = json.loads(subprocess.check_output(['hyprctl', 'clients', '-j']))
            client = next((c for c in current if c['pid'] == args.pid and c['address'] == key[0]
                           and tuple(c['size']) == key[1:]), None)
            if client is None:
                continue
            monitors = json.loads(subprocess.check_output(['hyprctl', 'monitors', '-j']))
            if client['workspace']['id'] not in {m['activeWorkspace']['id'] for m in monitors}:
                continue
            x, y = client['at']
            path = args.prefix.with_name(f'{args.prefix.name}-{len(seen) + 1}-{width}x{height}.png')
            subprocess.run(['grim', '-g', f'{x},{y} {width}x{height}', str(path)], check=True)
            path.with_suffix('.json').write_text(json.dumps(client, indent=2) + '\n')
            print(f'{client["title"]!r}: {path}', flush=True)
            seen.add(key)
        time.sleep(0.5)
    ✓ • 0ms

## Activity

    $ rg -n 'point.*rect|NSPointInRect|NSLog|replaceCharacters' tests/guest_native_geometry.m src/dylibs/Foundation/formatting.cpp src/dylibs/Foundation/strings.cpp src/dylibs/AppKit/text_view.cpp && sed -n '140,185p' tests/guest_native_geometry.m
    src/dylibs/AppKit/text_view.cpp:73:    send(contents(self), "replaceCharactersInRange:withString:", location, length, ret(text));
    src/dylibs/AppKit/text_view.cpp:174:        {"replaceCharactersInRange:withString:", MM_IMP(text_replace_range), "v40@0:8{_NSRange=QQ}16@32"},
    src/dylibs/Foundation/strings.cpp:854:std::uint64_t mm_replaceCharactersInRange(id self, SEL, std::uint64_t loc, std::uint64_t len, id s) { mutable_of(self, "replaceCharactersInRange:withString:")->chars.replace(loc, len, view(s)); return 0; }
    src/dylibs/Foundation/strings.cpp:1009:        {"replaceCharactersInRange:withString:", IMP(mm_replaceCharactersInRange), "v40@0:8{_NSRange=QQ}16@32"},
    tests/guest_native_geometry.m:7:extern BOOL exported_point_in_rect(NSPoint, NSRect) __asm__("_NSPointInRect");
    tests/guest_native_geometry.m:8:static BOOL (*volatile point_in_rect)(NSPoint, NSRect) = exported_point_in_rect;
    tests/guest_native_geometry.m:35:        if (!point_in_rect(NSMakePoint(50, 60), NSMakeRect(10, 20, 80, 90)) ||
    tests/guest_native_geometry.m:36:            point_in_rect(NSMakePoint(90, 60), NSMakeRect(10, 20, 80, 90)) ||
    tests/guest_native_geometry.m:37:            point_in_rect(NSMakePoint(50, 110), NSMakeRect(10, 20, 80, 90)) ||
    tests/guest_native_geometry.m:38:            point_in_rect(NSMakePoint(0, 0), NSZeroRect)) abort();
    ✓ • 0ms

## Activity

    $ rg -n -A 14 -B 3 'evilcocoa_renderer|Failed|expected|check:' work/logs/resumed-roblox-input-tests-host.log && ls -lt work/logs/roblox-proc-http-* | head -n 3 && tail -n 24 work/logs/roblox-proc-http-20261113-*.log | tail -n 30
    4-        Start   5: mevk_air_resources
    5-        Start   6: mevk_render
    6-  1/126 Test   #2: mevk_resource_reuse ..................   Passed    0.44 sec
    7:        Start   7: evilcocoa_renderer
    8-  2/126 Test   #6: mevk_render ..........................   Passed    0.51 sec
    9-        Start   9: mevk_guest_metal
    10-  3/126 Test   #5: mevk_air_resources ...................   Passed    0.51 sec
    11-        Start  12: mevk_guest_render
    12-  4/126 Test   #1: mevk_buffers .........................   Passed    0.60 sec
    13-        Start  21: wayland_clipboard
    14-  5/126 Test  #21: wayland_clipboard ....................   Passed    0.05 sec
    15-        Start  45: guest_opengl_threads_arm64
    16:  6/126 Test   #7: evilcocoa_renderer ...................***Failed    0.31 sec
    17-FAIL: hover settles into the selected fill and stops requesting frames
    18-
    19-        Start  46: guest_opengl_threads_x86_64
    20-  7/126 Test  #46: guest_opengl_threads_x86_64 ..........   Passed    0.14 sec
    21-        Start  94: guest_mach_ports_arm64
    22-  8/126 Test  #45: guest_opengl_threads_arm64 ...........   Passed    0.49 sec
    23-        Start  95: guest_mach_ports_x86_64
    24-  9/126 Test  #95: guest_mach_ports_x86_64 ..............   Passed    0.02 sec
    25-        Start  96: guest_mach_notifications_arm64
    26- 10/126 Test  #94: guest_mach_ports_arm64 ...............   Passed    0.27 sec
    27-        Start  97: guest_mach_exchange_arm64_arm64
    28- 11/126 Test   #9: mevk_guest_metal .....................   Passed    0.66 sec
    29-        Start  98: guest_mach_exchange_arm64_x86_64
    30- 12/126 Test  #12: mevk_guest_render ....................   Passed    0.70 sec
    --
    259-Total Test time (real) =   7.35 sec
    260-
    261-The following tests FAILED:
    262:      7 - evilcocoa_renderer (Failed)
    263-Errors while running CTest
    -rwxrwxrwx 1 nobody nobody  103795 Nov 13 04:59 work/logs/roblox-proc-http-20261113-045918.log
    -rwxrwxrwx 1 nobody nobody  151101 Nov 13 04:40 work/logs/roblox-proc-http-20261113-043847.log
    -rwxrwxrwx 1 nobody nobody  163212 Nov 13 04:25 work/logs/roblox-proc-http-20261113-041943.log
    [207148:1:20261113,044056.000477:INFO ClientCrashpadCallback.inl:220] Mac Client CrashHandler session UniqueId is: 2536412580653847345
    [207148:1:20261113,044056.001576:INFO CrashpadCallback.cpp:83] Will not add to session state
    run: entry returned x0=8590941872
    terminate called without an active exception
    
    ==> work/logs/roblox-proc-http-20261113-045918.log <==
    2026-11-13T01:59:53.552Z,20.552820,0001,6,Warning [FLog::Graphics] Using TexturePackGenerator.
    2026-11-13T01:59:53.552Z,20.552948,0001,6,Warning [FLog::Graphics] LightGrid is unified
    2026-11-13T01:59:53.589Z,20.589170,0001,6,Warning [FLog::Graphics] DRS init: deviceKey=MacBookPro18,1:NVIDIA GeForce RTX 4060: deviceDenied=false fullSizeFramebufferDenied=false canUseHighDPIFullFB=true
    2026-11-13T01:59:53.798Z,20.798288,0001,6 [FLog::SurfaceController] SurfaceController::disableVR
    2026-11-13T01:59:53.800Z,20.800732,0001,6,Warning [FLog::SurfaceController] SurfaceController[_:1]::initialize
    2026-11-13T01:59:53.802Z,20.802765,0001,6 [FLog::SurfaceController] SurfaceController[_:1]::start dataModel(0x7fd6e309fe78)
    2026-11-13T01:59:53.809Z,20.809286,0001,6 [FLog::SurfaceController] SurfaceController::RenderJob[_:1]::RenderJob
    2026-11-13T01:59:53.809Z,20.809492,0001,6,Warning [FLog::SurfaceController] SurfaceController::RenderJob::setStartupThrottle: true
    2026-11-13T01:59:53.809Z,20.809643,0001,6,Error [FLog::SurfaceController] SurfaceController::setStartupThrottle: preRenderJob is null
    2026-11-13T01:59:53.809Z,20.809935,0001,6 [FLog::SurfaceController] SurfaceController[_:1]::run dataModel(0x7fd6e309fe78)
    2026-11-13T01:59:53.816Z,20.816570,0001,6 [FLog::SingleSurfaceApp] setStage: (stage:LuaApp)
    2026-11-13T01:59:53.817Z,20.817318,0001,6 [FLog::SingleSurfaceApp] userDidLogin
    2026-11-13T01:59:53.817Z,20.817432,0001,6 [FLog::SingleSurfaceApp] applyLocale
    2026-11-13T01:59:53.818Z,20.818005,0001,6,Error [DFLog::RBBatteryStateLogger] Failed to copy power sources info.
    2026-11-13T01:59:53.818Z,20.818066,0014,6 [FLog::Output] (AppDelegate) Doing machine id check.
    2026-11-13T01:59:53.818Z,20.818274,0001,6 [FLog::RBThermalStateLogger] Start monitoring for thermal state.
    2026-11-13T01:59:53.818Z,20.818775,0001,6 [FLog::RBThermalStateLogger] Thermal state: Nominal
    2026-11-13T01:59:53.839Z,20.839775,0001,6,Info [FLog::MenuBar] Mac Device Id AA Test variable is not published
    2026-11-13T01:59:53.840Z,20.840031,0001,6,Info [FLog::MenuBar] Mac Device Id AB Test is Enabled.
    2026-11-13T01:59:53.877Z,20.877834,0004,6,Info [FLog::MenuBar] Launched successfully to menu bar
    2026-11-13T01:59:53.879Z,20.879034,0004,6,Info [FLog::UpdateController] UpdateController: versionQueryUrl: https://clientsettingscdn.roblox.com/v2/client-version/MacPlayer
    2026-11-13T01:59:53.879Z,20.879316,0004,6,Info [FLog::UpdateController] MacUpdateController: updaterFullPath: /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayerInstaller.app/Contents/MacOS/RobloxPlayerInstaller
    2026-11-13T01:59:53.999Z,20.999090,0011,12 [DFLog::HttpTraceError] HttpResponse(#24 0x7fd6d8dff100) time:173.8ms (net:172.8ms timeInRetryQueue:0.0ms)status:401 Unauthorized bodySize:82 url:{ "https://apis.roblox.com/realtime-replay-api/v1/config" } ip:128.116.5.3 external:0 numberOfTimesRetried:0
    2026-11-13T01:59:54.489Z,21.489424,0002,6 [DFLog::WebSocketTraceError] ws: 0 url: https://realtime
    ✓ • 0ms

## Activity

    $ python3 -B work/capture-live-windows.py --pid 223871 --prefix work/logs/roblox-resumed-input --seconds 3
    'Roblox': work/logs/roblox-resumed-input-1-949x509.png
    'Roblox': work/logs/roblox-resumed-input-2-949x1028.png
    ✓ • 3.36s

## Activity

    $ tail -n 15 work/logs/roblox-proc-http-20261113-045918.log
    2026-11-13T02:00:08.867Z,35.867378,0005,6,Debug [DFLog::FlagCache] [FlagCache] writeFlagCache: Wrote signature of length 88. Signature: xZ4c...
    2026-11-13T02:00:08.867Z,35.867516,0005,6,Debug [DFLog::FlagCache] [FlagCache] writeFlagCache: Wrote compression type: 1
    2026-11-13T02:00:08.867Z,35.867634,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Compressing flag cache data (input size: 1469992 bytes)
    2026-11-13T02:00:08.876Z,35.876869,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Compression complete. Output size: 411049 bytes, ratio: 3.58x, time: 9ms
    2026-11-13T02:00:08.877Z,35.877205,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Successfully wrote 411142 bytes to file: /tmp/Roblox/cache/flag_cache.dat, time: 10.15174599999824ms
    2026-11-13T02:00:08.877Z,35.877880,0013,6,Info [FLog::TombstoneCache] [FlagCache] Tombstone 1, expiry time 360, holdout false, channel '' (production), written to file: /tmp/Roblox/cache/tombstone.dat
    2026-11-13T02:00:08.939Z,35.939789,0005,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://users.roblox.com/v1/users/authenticated/app-launch-info`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://users.roblox.com/v1/users/authenticated/app-launch-info`
    2026-11-13T02:00:08.941Z,35.941242,0005,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`401`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`401`
    2026-11-13T02:00:08.942Z,35.942734,0005,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`{"errors":[{"code":9002,"subcode":0,"message":"Authentication token is missing"}]}`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`{"errors":[{"code":9002,"subcode":0,"message":"Authentication token is missing"}]}`
    2026-11-13T02:00:09.347Z,36.347527,0001,6 [FLog::Graphics] SceneManager: resizing main targets to 949x1000
    2026-11-13T02:00:11.188Z,38.188236,0011,12 [DFLog::HttpTraceError] HttpResponse(#52 0x7fd682290100) time:169.5ms (net:168.5ms timeInRetryQueue:0.0ms)status:401  bodySize:82 url:{ "https://voice.roblox.com/v4/turn/all-regions/" } ip:128.116.5.3 external:0 numberOfTimesRetried:0
    2026-11-13T02:00:11.188Z,38.188988,0011,6,Warning [DFLog::TurnApiClient] TurnApiClient: HTTP failure (err=0, s
    ✓ • 0ms

## Activity

    $ rg -n -A 18 -B 24 'hover settles' modules/evilcocoa/tests/test_renderer.cpp && rg -n 'hover|animation|steady_clock|frame' modules/evilcocoa/src/renderer.cpp | head -n 55
    120-        picture.frame = {10, 10, 40, 40};
    121-        picture.clip = {0, 0, 240, 140};
    122-        picture.pixels = bitmap;
    123-        picture.scaling = evilcocoa::image_scaling::stretch;
    124-        renderer.draw(std::span(&picture, 1), 240, 140);
    125-        auto colored = read();
    126-        auto channel = [&](int x, int y, int c) { return colored[(std::size_t(139 - y) * 240 + x) * 4 + c]; };
    127-        require(channel(12, 12, 0) > 230 && channel(12, 12, 1) < 10, "full-color image keeps red");
    128-        require(channel(48, 12, 1) > 230 && channel(48, 12, 0) < 10, "image orientation and green channel");
    129-        require(channel(12, 48, 2) > 230, "image bottom row remains below top row");
    130-        require(channel(48, 48, 0) > 130 && channel(48, 48, 0) < 170,
    131-                "premultiplied alpha blends without multiplying twice");
    132-        evilcocoa::item menu;
    133-        menu.type = evilcocoa::kind::menu_item;
    134-        menu.frame = {10,50,180,30}; menu.clip = {0,0,240,140};
    135-        renderer.draw(std::span(&menu,1),240,140);
    136-        auto resting = read();
    137-        menu.pressed = true;
    138-        renderer.draw(std::span(&menu,1),240,140);
    139-        require(renderer.animating(),"hover transition requests subsequent frames");
    140-        std::this_thread::sleep_for(std::chrono::milliseconds(140));
    141-        renderer.draw(std::span(&menu,1),240,140);
    142-        auto hovered = read();
    143-        require(!renderer.animating() && pixel(hovered,30,65)>pixel(resting,30,65),
    144:                "hover settles into the selected fill and stops requesting frames");
    145-        for (auto kind :
    146-             {evilcocoa::kind::checkbox, evilcocoa::kind::radio, evilcocoa::kind::popup, evilcocoa::kind::combo_box,
    147-              evilcocoa::kind::slider, evilcocoa::kind::progress, evilcocoa::kind::scroller,
    148-              evilcocoa::kind::scroll_view, evilcocoa::kind::table_row, evilcocoa::kind::segments}) {
    149-            evilcocoa::item control;
    150-            control.type = kind;
    151-            control.frame = {10, 50, 180, 30};
    152-            control.clip = {0, 0, 240, 140};
    153-            control.text = "Control";
    154-            control.value = .5;
    155-            control.checked = true;
    156-            control.segments = {"One", "Two"};
    157-            control.selected = 1;
    158-            renderer.draw(std::span(&control, 1), 240, 140);
    159-            auto pixels = read();
    160-            int changed = 0;
    161-            for (int y = 50; y < 80; ++y)
    162-                for (int x = 10; x < 190; ++x)
    29:uniform vec4 frame;
    34:    vec2 p = frame.xy + uv * frame.zw;
    76:    GLint viewport, frame, extent, radius, padding, softness, ink, shade, segment, textured;
    83:    struct highlight_animation {
    84:        rect frame;
    89:    std::unordered_map<std::size_t,highlight_animation> menu_highlights;
    90:    bool animations_running = false;
    93:        auto [it,inserted] = menu_highlights.try_emplace(index,highlight_animation{i.frame,i.text,target,target,now});
    95:        if (!inserted && (a.frame != i.frame || a.text != i.text)) a = {i.frame,i.text,target,target,now};
    101:        if (now-a.changed < .12 && a.from != a.target) animations_running = true;
    120:        frame = glGetUniformLocation(program, "frame");
    146:        glUniform4f(frame, float(r.x), float(r.y), float(r.w), float(r.h));
    276:bool renderer::animating() const { return impl_->animations_running; }
    281:    p.animations_running = false;
    282:    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    312:        const rect r{i.frame.x * scale, i.frame.y * scale, i.frame.w * scale, i.frame.h * scale};
    ✓ • 0ms

## Activity

    Viewed image /mnt/media/machoman/work/logs/roblox-resumed-input-1-949x509.png

## Assistant

Roblox запущен с исправлениями. Проверь, пожалуйста, ввод в полях, клики мышью и отправку формы входа. Что сейчас работает, а где остаётся проблема? Пароль присылать не нужно.

## Activity

    $ python3 -B work/debug-roblox-cookies.py --interactive
    Roblox PID 223871
    Interactive login run: no timeout or automatic screenshots
    Saved work/logs/roblox-proc-http-20261113-045918 diagnostics
    ✓ • 1m 09s

## User

> Roblox запущен с исправлениями. Проверь, пожалуйста, ввод в полях, клики мышью и отправку формы входа. Что сейчас работает, а где остаётся проблема? Пароль присылать не нужно.

все работает, теперь давай разберем краш при нажатии sign in после ввода пароля. думаю придется делать имплементацию вебвью

## Assistant

Ввод и клики подтверждены. Теперь разберу падение при Sign In: сначала проверю стек и последний вызов, чтобы понять, какие части WebView нужны этому пути входа.

## Activity

    $ sed -n '1,200p' src/dylibs/symbol-synthesis/file_types.h && sed -n '1,165p' src/dylibs/symbol-synthesis/synthesise.cpp && sed -n '1,100p' modules/evilcocoa/tests/test_renderer.cpp && sed -n '75,108p' modules/evilcocoa/src/renderer.cpp
    #ifndef MACHOMAN_FILE_TYPES_H
    #define MACHOMAN_FILE_TYPES_H
    
    #include <string_view>
    
    /* AVFoundation's file type constants are UTI identifiers, rather than
     * opaque keys. CoreServices uses the same identifiers for descriptions. */
    struct mm_av_file_type {
        std::string_view symbol, identifier, description, parent;
    };
    inline constexpr mm_av_file_type mm_av_file_types[] = {
        {"AVFileTypeQuickTimeMovie", "com.apple.quicktime-movie", "QuickTime movie", "public.movie"},
        {"AVFileTypeMPEG4", "public.mpeg-4", "MPEG-4 movie", "public.movie"},
        {"AVFileTypeAppleM4V", "com.apple.m4v-video", "MPEG-4 video", "public.mpeg-4"},
        {"AVFileTypeAppleM4A", "com.apple.m4a-audio", "MPEG-4 audio", "public.audio"},
        {"AVFileType3GPP", "public.3gpp", "3GPP movie", "public.movie"},
        {"AVFileType3GPP2", "public.3gpp2", "3GPP2 movie", "public.movie"},
        {"AVFileTypeCoreAudioFormat", "com.apple.coreaudio-format", "Core Audio format", "public.audio"},
        {"AVFileTypeWAVE", "com.microsoft.waveform-audio", "Waveform audio", "public.audio"},
        {"AVFileTypeAIFF", "public.aiff-audio", "AIFF audio", "public.aifc-audio"},
        {"AVFileTypeAIFC", "public.aifc-audio", "AIFC audio", "public.audio"},
        {"AVFileTypeAMR", "org.3gpp.adaptive-multi-rate-audio", "AMR audio", "public.audio"},
        {"AVFileTypeMPEGLayer3", "public.mp3", "MP3 audio", "public.audio"},
        {"AVFileTypeAC3", "public.ac3-audio", "AC-3 audio", "public.audio"},
        {"AVFileTypeEnhancedAC3", "public.enhanced-ac3-audio", "Enhanced AC-3 audio", "public.audio"},
        {"AVFileTypeJPEG", "public.jpeg", "JPEG image", "public.image"},
        {"AVFileTypeDNG", "com.adobe.raw-image", "Digital Negative image", "public.camera-raw-image"},
        {"AVFileTypeHEIC", "public.heic", "HEIF image", "public.heif"},
        {"AVFileTypeAVCI", "public.avci", "AVC image", "public.image"},
        {"AVFileTypeHEIF", "public.heif", "HEIF image", "public.image"},
        {"AVFileTypeTIFF", "public.tiff", "TIFF image", "public.image"},
    };
    
    #endif
    /* Synthesising a framework symbol from its name.
     *
     * Shared by the stub framework (which stands in for every macOS framework
     * machoman does not implement, linked under each of their paths) and by the
     * real frameworks, for the constants they have not defined yet: the runtime
     * asks a host library for each bind it cannot resolve statically through
     * `machoman_resolve_symbol`, and this answers:
     *
     *   _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X  a real objc class named X,
     *                                          subclass of NSObject, so the
     *                                          guest can reference and
     *                                          subclass it; a message it does
     *                                          not inherit uses libobjc's
     *                                          typed, stateful compatibility
     *                                          methods when metadata exists
     *   k…, …Key, …Name, …Domain, …Notification, …                  an NSString
     *                                          whose value is the symbol name
     *                                          (Apple's constants are opaque
     *                                          tokens; the name is a unique,
     *                                          printable, comparable one)
     *   everything else                        a function that reports the
     *                                          call once and returns 0
     *
     * This keeps unimplemented frameworks honest: a program only fails when it
     * actually depends on behaviour, and the message says which symbol it was.
     * $MACHOMAN_STUB_TRACE=1 logs every synthesised symbol. */
    #include "machoman/objc_host.h"
    #include "machoman/runtime.h"
    #include "file_types.h"
    
    #include <cstdint>
    #include <atomic>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <dlfcn.h>
    #include <mutex>
    #include <string>
    #include <string_view>
    #include <sys/mman.h>
    #include <unordered_map>
    #include <vector>
    
    #define EXPORT __attribute__((visibility("default")))
    
    #include "synthesise.h"
    
    namespace {
    
    std::mutex g_mu;
    std::unordered_map<std::string, void *> g_cache;
    
    bool trace() { static const bool on = std::getenv("MACHOMAN_STUB_TRACE") != nullptr; return on; }
    
    /* ---- function stubs ------------------------------------------------------ */
    
    struct stub_entry { std::string symbol; std::atomic<bool> reported{false}; };
    
    extern "C" std::uint64_t mm_stub_report(stub_entry *e) {
        if (!e->reported.load(std::memory_order_relaxed) && !e->reported.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "stub: [risk:unknown-abi] %s is not implemented (zero register result; pointer/structure/out-parameter contract unknown)\n", e->symbol.c_str());
        return 0;
    }
    
    /* One trampoline per symbol, cut from a shared executable page. Reporting
     * has one relaxed atomic flag per entry and no warm-path lock/hash lookup;
     * the trampoline clears integer and FP results after the report. */
    void *make_function_stub(const std::string &symbol) {
        static std::uint8_t *page = nullptr;
        static std::size_t used = 4096;
        constexpr std::size_t slot = 64;
        if (used + slot > 4096) {
            void *p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) return nullptr;
            page = static_cast<std::uint8_t *>(p);
            used = 0;
        }
        std::uint8_t *code = page + used;
        used += slot;
    
        auto *e = new stub_entry{symbol};
        std::size_t i = 0;
        code[i++] = 0x48; code[i++] = 0xBF;                       /* movabs rdi, imm64 */
        const auto arg = reinterpret_cast<std::uint64_t>(e);
        std::memcpy(code + i, &arg, 8); i += 8;
        code[i++] = 0x48; code[i++] = 0xB8;                       /* movabs rax, imm64 */
        const auto fn = reinterpret_cast<std::uint64_t>(&mm_stub_report);
        std::memcpy(code + i, &fn, 8); i += 8;
        const std::uint8_t tail[] = {
            0x48, 0x83, 0xEC, 0x08, /* sub rsp, 8: align before call */
            0xFF, 0xD0,             /* call rax */
            0x48, 0x83, 0xC4, 0x08, /* add rsp, 8 */
            0x31, 0xC0,             /* xor eax, eax */
            0x66, 0x0F, 0xEF, 0xC0, /* pxor xmm0, xmm0 */
            0x66, 0x0F, 0xEF, 0xC9, /* pxor xmm1, xmm1 */
            0xC3
        };
        std::memcpy(code + i, tail, sizeof tail);
        return code;
    }
    
    /* ---- classes and constants ------------------------------------------------ */
    
    /* Class pairs are allocated here because the guest binds to their address. */
    struct class_pair { mm_objc_class cls, meta; };
    
    mm_objc_class *stub_class(const std::string &name, bool want_meta) {
        /* An umbrella framework re-exports the classes its members define, so
         * a class that already exists is that class, not a new stub. */
        if (mm_objc_class *existing = mm_objc_class_named(name.c_str()))
            return want_meta ? mm_objc_object_class(reinterpret_cast<mm_id>(existing)) : existing;
        static std::unordered_map<std::string, class_pair *> classes;
        auto it = classes.find(name);
        if (it == classes.end()) {
            auto *p = new class_pair{};
            mm_objc_class *nsobject = mm_objc_class_named("NSObject");
            mm_objc_define_class(name.c_str(), nsobject, sizeof(void *), &p->cls, &p->meta);
            mm_objc_enable_dynamic_stubs(&p->cls);
            it = classes.emplace(name, p).first;
        }
        return want_meta ? &it->second->meta : &it->second->cls;
    }
    
    /* A constant like `extern NSString *const kFoo` is a *variable*: the guest
     * loads the pointer out of it, so what a bind needs is the address of a
     * cell holding the object, not the object itself. The value is an NSString
     * of the symbol's own name for opaque keys. File type constants instead
     * carry their public UTI values, consumed by CoreServices. */
    void *constant_string(const std::string &name) {
        mm_objc_class *nsstring = mm_objc_class_named("NSString");
        if (!nsstring) return nullptr;
        std::string_view text(name);
        for (const auto &type : mm_av_file_types)
            if (type.symbol == name) { text = type.identifier; break; }
        const std::uint64_t arg = reinterpret_cast<std::uintptr_t>(text.data());
        mm_id s = reinterpret_cast<mm_id>(mm_objc_send(reinterpret_cast<mm_id>(nsstring), mm_objc_sel("stringWithUTF8String:"), 1, &arg));
        if (!s) return nullptr;
        auto *cell = new mm_id(mm_objc_retain(s));
        return cell;
    }
    
    /* Function or constant? A lazy bind is a call site, so the symbol is a
     * function; any other bind reaches the guest through the GOT, which is how
     * data constants are read. Guessing from the name instead is unreliable —
     * constants are full of verbs ("NSApplicationDidBecomeActiveNotification",
     * "AVLinearPCMIsFloatKey") — and getting it wrong turns a string into code
     * or code into a string, so the bind record decides. A function imported by
     * address rather than called is the one case this misreads; it then behaves
     * as a constant and the guest fails where it calls it. */
    bool looks_like_function(unsigned flags) { return (flags & MM_SYMBOL_LAZY) != 0; }
    
    void *lookup_gl_symbol(const std::string &name) {
        if (!name.starts_with("gl")) return nullptr;
        static void *libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (libgl) {
            if (void *sym = dlsym(libgl, name.c_str())) return sym;
        }
        using proc_fn = void (*)();
        static auto egl_get_proc = reinterpret_cast<proc_fn (*)(const char *)>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
        if (egl_get_proc) {
            if (void *sym = reinterpret_cast<void *>(egl_get_proc(name.c_str()))) return sym;
        }
        return nullptr;
    }
    
    #include <array>
    #include <cstdio>
    #include <cstdlib>
    #include <chrono>
    #include <thread>
    #include <epoxy/egl.h>
    #include <epoxy/gl.h>
    #include <evilcocoa/renderer.h>
    #include <vector>
    void require(bool value, const char *message) {
        if (!value) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }
    int main() {
        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        require(eglInitialize(display, nullptr, nullptr), "surfaceless EGL display");
        eglBindAPI(EGL_OPENGL_API);
        EGLint attributes[] = {EGL_SURFACE_TYPE,
                               EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_BIT,
                               EGL_RED_SIZE,
                               8,
                               EGL_GREEN_SIZE,
                               8,
                               EGL_BLUE_SIZE,
                               8,
                               EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
        EGLint size[] = {EGL_WIDTH, 240, EGL_HEIGHT, 140, EGL_NONE};
        EGLSurface surface = eglCreatePbufferSurface(display, config, size);
        EGLint context_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION,           3,       EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
        require(eglMakeCurrent(display, surface, surface, context), "OpenGL 3.3 current context");
        {
            evilcocoa::renderer renderer;
            evilcocoa::item label;
            label.frame = {10, 8, 220, 28};
            label.clip = {0, 0, 240, 140};
            label.text = "Привет · Cocoa";
            evilcocoa::item button;
            button.type = evilcocoa::kind::button;
            button.frame = {10, 50, 140, 40};
            button.clip = {0, 0, 240, 140};
            button.text = "Open folder";
            auto read = [&] {
                std::vector<unsigned char> p(240 * 140 * 4);
                glReadPixels(0, 0, 240, 140, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
                require(glGetError() == GL_NO_ERROR, "valid GL draw");
                return p;
            };
            auto pixel = [](const auto &p, int x, int y) { return p[(std::size_t(139 - y) * 240 + x) * 4]; };
            std::array items{label, button};
            renderer.draw(items, 240, 140);
            auto normal = read();
            require(pixel(normal, 15, 70) > pixel(normal, 200, 120), "button fill visible");
            require(pixel(normal, 20, 56) > pixel(normal, 20, 84), "button has a vertical gradient");
            require(pixel(normal, 80, 91) < pixel(normal, 200, 120), "shadow extends below the button");
            int text_pixels = 0;
            for (int y = 8; y < 36; y++)
                for (int x = 10; x < 230; x++)
                    text_pixels += pixel(normal, x, y) > 180;
            require(text_pixels > 40, "Unicode glyphs composited");
            items[0].text_color = {1.f,0.f,0.f,1.f};
            renderer.draw(items,240,140);
            auto red_text = read();
            int red_glyphs = 0;
            for (int y=8; y<36; ++y) for (int x=10; x<230; ++x) {
                const auto offset=(std::size_t(139-y)*240+x)*4;
                red_glyphs += red_text[offset]>180 && red_text[offset+1]<80 && red_text[offset+2]<80;
            }
            require(red_glyphs>40,"attributed foreground colors tint glyphs");
            items[0].text_color.reset();
            items[1].pressed = true;
            renderer.draw(items, 240, 140);
            auto pressed = read();
            require(pixel(pressed, 15, 70) < pixel(normal, 15, 70), "pressed state changes fill");
            items[1].pressed = false;
            items[1].enabled = false;
            renderer.draw(items, 240, 140);
            auto disabled = read();
            require(disabled != normal, "disabled text state");
            items[1].clip = {0, 0, 5, 5};
            renderer.draw(items, 240, 140);
            auto clipped = read();
            require(pixel(clipped, 15, 70) == pixel(clipped, 200, 120), "clip excludes control");
            require(pixel(clipped, 80, 91) == pixel(clipped, 200, 120), "containing clip also excludes shadow");
            renderer.draw(items, 240, 140, 2);
            read();
            evilcocoa::item centered;
            centered.frame = {10, 10, 200, 25};
            centered.clip = {0, 0, 240, 140};
            centered.text = "Center";
            centered.align = evilcocoa::alignment::center;
        GLuint program = 0, vao = 0;
        GLint viewport, frame, extent, radius, padding, softness, ink, shade, segment, textured;
        struct cached_image {
            std::shared_ptr<const bitmap> pixels;
            texture gl;
        };
        std::vector<cached_image> images;
        std::unordered_map<std::string, texture> text;
        struct highlight_animation {
            rect frame;
            std::string text;
            float from = 0, target = 0;
            double changed = 0;
        };
        std::unordered_map<std::size_t,highlight_animation> menu_highlights;
        bool animations_running = false;
        float menu_highlight(std::size_t index, const item &i, double now) {
            const float target = i.pressed && i.enabled ? 1.f : 0.f;
            auto [it,inserted] = menu_highlights.try_emplace(index,highlight_animation{i.frame,i.text,target,target,now});
            auto &a = it->second;
            if (!inserted && (a.frame != i.frame || a.text != i.text)) a = {i.frame,i.text,target,target,now};
            auto amount = [&] {
                const float t = float(std::clamp((now-a.changed)/.12,0.,1.));
                return a.from + (a.target-a.from)*t*t*(3-2*t);
            };
            if (a.target != target) { a.from = amount(); a.target = target; a.changed = now; }
            if (now-a.changed < .12 && a.from != a.target) animations_running = true;
            return amount();
        }
        impl() {
            auto vs = shader(GL_VERTEX_SHADER, vertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
            program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
    ✓ • 267ms

## Activity

    $ tail -n 85 work/logs/roblox-proc-http-20261113-045918.log && rg -n 'WebKit|WKWeb|WebView|NSLog|loadRequest|navigation|captcha' src/dylibs/symbol-synthesis/*.h src/dylibs/symbol-synthesis/*.cpp work/logs/roblox-proc-http-20261113-045918.log | tail -n 65
    2026-11-13T02:00:08.049Z,35.049622,0011,12 [DFLog::HttpTraceError] HttpResponse(#45 0x7fd687fec100) time:162.6ms (net:161.2ms timeInRetryQueue:0.0ms)status:401 Unauthorized bodySize:82 url:{ "https://users.roblox.com/v1/users/authenticated/app-launch-info" } ip:128.116.5.3 external:0 numberOfTimesRetried:0
    2026-11-13T02:00:08.066Z,35.066708,0011,12 [DFLog::HttpTraceError] HttpResponse(#46 0x7fd687fecd00) time:165.1ms (net:164.3ms timeInRetryQueue:0.0ms)status:401 Unauthorized bodySize:82 url:{ "https://apis.roblox.com/user-settings-api/v1/user-settings" } ip:128.116.5.3 external:0 numberOfTimesRetried:0
    2026-11-13T02:00:08.067Z,35.067932,0004,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://apis.roblox.com/attribution/v1/events/post-authentication`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://apis.roblox.com/attribution/v1/events/post-authentication`
    2026-11-13T02:00:08.069Z,35.069401,0004,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`nil`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`nil`
    2026-11-13T02:00:08.070Z,35.070877,0004,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`nil`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`nil`
    2026-11-13T02:00:08.076Z,35.076450,0004,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://users.roblox.com/v1/users/authenticated/app-launch-info`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://users.roblox.com/v1/users/authenticated/app-launch-info`
    2026-11-13T02:00:08.077Z,35.077919,0004,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`401stub: [risk:unknown-abi] _CGAssociateMouseAndMouseCursorPosition is not implemented (zero register result; pointer/structure/out-parameter contract unknown)
    AppKit: pointer down window=0x56092965eb90 local={505.961,184.961} screen={805.961,484.961}
    AppKit: mouseDown: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer up window=0x56092965eb90 local={505.961,184.961} screen={805.961,484.961}
    AppKit: mouseUp: -> RobloxOgreView 0x56092a1ddd70
    `
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`401`
    2026-11-13T02:00:08.079Z,35.079540,0004,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`{"errors":[{"code":9002,"subcode":0,"message":"Authentication token is missing"}]}`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`{"errors":[{"code":9002,"subcode":0,"message":"Authentication token is missing"}]}`
    2026-11-13T02:00:08.501Z,35.501114,0004,6,Warning [DFLog::DMNotificationMonitor] DM notification received. Type 10
    2026-11-13T02:00:08.769Z,35.769291,0011,12 [DFLog::HttpTraceError] HttpResponse(#49 0x7fd688e76700) time:169.7ms (net:164.2ms timeInRetryQueue:0.0ms)status:401 Unauthorized bodySize:82 url:{ "https://users.roblox.com/v1/users/authenticated/app-launch-info" } ip:128.116.5.3 external:0 numberOfTimesRetried:0
    2026-11-13T02:00:08.865Z,35.865948,0004,6,Info [DFLog::FlagCache] [FlagCache] Writing pending flag cache to disk
    2026-11-13T02:00:08.866Z,35.866589,0004,6,Warning [FLog::SingleSurfaceApp] Restoring rendering frequency to normal.
    2026-11-13T02:00:08.866Z,35.866711,0004,6,Warning [FLog::SurfaceController] SurfaceController::RenderJob::setStartupThrottle: false
    2026-11-13T02:00:08.866Z,35.866821,0004,6,Error [FLog::SurfaceController] SurfaceController::setStartupThrottle: preRenderJob is null
    2026-11-13T02:00:08.867Z,35.867054,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Writing flag cache file to : /tmp/Roblox/cache/flag_cache.dat
    2026-11-13T02:00:08.867Z,35.867249,0005,6,Debug [DFLog::FlagCache] [FlagCache] writeFlagCache: Wrote signatureSize: 88
    2026-11-13T02:00:08.867Z,35.867378,0005,6,Debug [DFLog::FlagCache] [FlagCache] writeFlagCache: Wrote signature of length 88. Signature: xZ4c...
    2026-11-13T02:00:08.867Z,35.867516,0005,6,Debug [DFLog::FlagCache] [FlagCache] writeFlagCache: Wrote compression type: 1
    2026-11-13T02:00:08.867Z,35.867634,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Compressing flag cache data (input size: 1469992 bytes)
    2026-11-13T02:00:08.876Z,35.876869,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Compression complete. Output size: 411049 bytes, ratio: 3.58x, time: 9ms
    2026-11-13T02:00:08.877Z,35.877205,0005,6,Info [DFLog::FlagCache] [FlagCache] writeFlagCache: Successfully wrote 411142 bytes to file: /tmp/Roblox/cache/flag_cache.dat, time: 10.15174599999824ms
    2026-11-13T02:00:08.877Z,35.877880,0013,6,Info [FLog::TombstoneCache] [FlagCache] Tombstone 1, expiry time 360, holdout false, channel '' (production), written to file: /tmp/Roblox/cache/tombstone.dat
    2026-11-13T02:00:08.939Z,35.939789,0005,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://users.roblox.com/v1/users/authenticated/app-launch-info`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error url:`https://users.roblox.com/v1/users/authenticated/app-launch-info`
    2026-11-13T02:00:08.941Z,35.941242,0005,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`401`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error code:`401`
    2026-11-13T02:00:08.942Z,35.942734,0005,6,Warning [FLog::CreatorWarning] Warning: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`{"errors":[{"code":9002,"subcode":0,"message":"Authentication token is missing"}]}`
    WARNING: [Roblox][CorePackages.LuauForgeChunks.bundled] HTTP error body:`{"errors":[{"code":9002,"subcode":0,"message":"Authentication token is missing"}]}`
    2026-11-13T02:00:09.347Z,36.347527,0001,6 [FLog::Graphics] SceneManager: resizing main targets to 949x1000
    2026-11-13T02:00:11.188Z,38.188236,0011,12 [DFLog::HttpTraceError] HttpResponse(#52 0x7fd682290100) time:169.5ms (net:168.5ms timeInRetryQueue:0.0ms)status:401  bodySize:82 url:{ "https://voice.roblox.com/v4/turn/all-regions/" } ip:128.116.5.3 external:0 numberOfTimesRetried:0
    2026-11-13T02:00:11.188Z,38.188988,0011,6,Warning [DFLog::TurnApiClient] TurnApiClient: HTTP failure (err=0, sAppKit: pointer down window=0x56092965eb90 local={397.977,433.805} screen={697.977,733.805}
    AppKit: mouseDown: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer up window=0x56092965eb90 local={397.977,433.805} screen={697.977,733.805}
    AppKit: mouseUp: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer down window=0x56092965eb90 local={636.23,432.062} screen={936.23,732.062}
    AppKit: mouseDown: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer up window=0x56092965eb90 local={636.23,432.062} screen={936.23,732.062}
    AppKit: mouseUp: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer down window=0x56092965eb90 local={649.309,428.336} screen={949.309,728.336}
    AppKit: mouseDown: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer up window=0x56092965eb90 local={649.309,428.336} screen={949.309,728.336}
    AppKit: mouseUp: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer down window=0x56092965eb90 local={553.848,420.023} screen={853.848,720.023}
    AppKit: mouseDown: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer up window=0x56092965eb90 local={553.848,420.023} screen={853.848,720.023}
    AppKit: mouseUp: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer down window=0x56092965eb90 local={503.953,356.961} screen={803.953,656.961}
    AppKit: mouseDown: -> RobloxOgreView 0x56092a1ddd70
    AppKit: pointer up window=0x56092965eb90 local={503.953,356.961} screen={803.953,656.961}
    AppKit: mouseUp: -> RobloxOgreView 0x56092a1ddd70
    stub: [risk:unknown-abi] -[NSCursor setOnMouseEntered:] Q@:Q
    stub: [risk:unknown-abi] -[NSCursor setOnMouseExited:] Q@:Q
    stub: [risk:unknown-abi] +[NSButton buttonWithImage:target:action:] Q@:QQQ
    stub: [risk:unknown-abi] +[NSTextField labelWithString:] Q@:Q
    stub: [risk:unknown-abi] -[NSBox setFillColor:] Q@:Q
    stub: [risk:unknown-abi] -[NSBox setContentViewMargins:] Q@:Q
    stub: [risk:unknown-abi] -[NSStackView setDetachesHiddenViews:] Q@:Q
    stub: [risk:unknown-abi] -[NSStackView setEdgeInsets:] Q@:Q
    stub: [risk:unknown-abi] -[NSProgressIndicator setStyle:] Q@:Q
    stub: [risk:inferred-abi] -[WKWebViewConfiguration websiteDataStore] @@:
    stub: [risk:inferred-abi] -[WKWebViewConfiguration httpCookieStore] @@:
    stub: [risk:inferred-abi] -[WKWebViewConfiguration setCookie:completionHandler:] Q@:QQ
    stub: [risk:unknown-abi] -[NSBox setBorderColor:] Q@:Q
    stub: [risk:unknown-abi] +[NSImage imageWithSize:flipped:drawingHandler:] Q@:QQQ
    stub: [risk:inferred-abi] -[WKWebViewConfiguration setApplicationNameForUserAgent:] v@:Q
    stub: [risk:inferred-abi] -[WKWebViewConfiguration userContentController] @@:
    stub: [risk:inferred-abi] -[WKWebViewConfiguration addScriptMessageHandler:name:] Q@:QQ
    stub: [risk:inferred-abi] -[WKWebView initWithFrame:configuration:] @@:QQ
    stub: [risk:inferred-abi] -[WKWebView setNavigationDelegate:] v@:Q
    stub: [risk:inferred-abi] -[WKWebView setUIDelegate:] v@:Q
    [224013:1:20261113,050027.529008:INFO ClientCrashpadCallback.inl:220] Mac Client CrashHandler session UniqueId is: 2578058688710073956
    [224013:1:20261113,050027.530056:INFO CrashpadCallback.cpp:83] Will not add to session state
    run: entry returned x0=8590941872
    terminate called without an active exception
    work/logs/roblox-proc-http-20261113-045918.log:1840:stub: [risk:inferred-abi] -[WKWebViewConfiguration websiteDataStore] @@:
    work/logs/roblox-proc-http-20261113-045918.log:1841:stub: [risk:inferred-abi] -[WKWebViewConfiguration httpCookieStore] @@:
    work/logs/roblox-proc-http-20261113-045918.log:1842:stub: [risk:inferred-abi] -[WKWebViewConfiguration setCookie:completionHandler:] Q@:QQ
    work/logs/roblox-proc-http-20261113-045918.log:1845:stub: [risk:inferred-abi] -[WKWebViewConfiguration setApplicationNameForUserAgent:] v@:Q
    work/logs/roblox-proc-http-20261113-045918.log:1846:stub: [risk:inferred-abi] -[WKWebViewConfiguration userContentController] @@:
    work/logs/roblox-proc-http-20261113-045918.log:1847:stub: [risk:inferred-abi] -[WKWebViewConfiguration addScriptMessageHandler:name:] Q@:QQ
    work/logs/roblox-proc-http-20261113-045918.log:1848:stub: [risk:inferred-abi] -[WKWebView initWithFrame:configuration:] @@:QQ
    work/logs/roblox-proc-http-20261113-045918.log:1849:stub: [risk:inferred-abi] -[WKWebView setNavigationDelegate:] v@:Q
    work/logs/roblox-proc-http-20261113-045918.log:1850:stub: [risk:inferred-abi] -[WKWebView setUIDelegate:] v@:Q
    ✓ • 275ms

## Activity

    $ coredumpctl --no-pager list /tmp/machoman-roblox-input-build/machoman
    TIME                           PID  UID  GID SIG     COREFILE EXE                                         SIZE
    Fri 2026-11-13 04:16:37 MSK 191577 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman   1.5M
    Fri 2026-11-13 04:25:22 MSK 193152 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman 240.7M
    Fri 2026-11-13 04:25:24 MSK 193278 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman  13.4M
    Fri 2026-11-13 04:38:38 MSK 206789 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman     1M
    Fri 2026-11-13 04:40:21 MSK 208536 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman  42.6M
    Fri 2026-11-13 04:40:55 MSK 206929 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman 208.9M
    Fri 2026-11-13 04:40:57 MSK 207148 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman  13.4M
    Fri 2026-11-13 04:43:35 MSK 212173 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman  42.6M
    Fri 2026-11-13 04:47:41 MSK 215058 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman     1M
    Fri 2026-11-13 04:50:58 MSK 217029 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman  42.3M
    Fri 2026-11-13 04:53:08 MSK 218051 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman     1M
    Fri 2026-11-13 04:57:55 MSK 222203 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman     3M
    Fri 2026-11-13 04:57:55 MSK 222228 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman   3.1M
    Fri 2026-11-13 04:57:58 MSK 222476 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman  82.4K
    Fri 2026-11-13 04:57:59 MSK 222475 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman 118.6K
    Fri 2026-11-13 05:00:27 MSK 223871 1000 1000 SIGSEGV present  /tmp/machoman-roblox-input-build/machoman   206M
    Fri 2026-11-13 05:00:28 MSK 224013 1000 1000 SIGABRT present  /tmp/machoman-roblox-input-build/machoman  13.4M
    ✓ • 45ms

## Activity

    file changes: Completed · 1 changes
    Add: /mnt/media/machoman/work/roblox-sign-in-core.gdb
    set pagination off
    set debuginfod enabled off
    set print frame-arguments none
    thread apply all bt 16
    info registers rip rsp rax rdi rsi rdx
    x/8i $pc

## Activity

    $ rg -n 'NSLog|ns_log|NSPointInRect' src/dylibs/Foundation/*.cpp && sed -n '1,240p' src/dylibs/libobjc/native_abi.h
    src/dylibs/Foundation/misc.cpp:2261:/* NSLog and the Foundation free functions. */
    src/dylibs/Foundation/misc.cpp:2262:void NSLog_(id fmt, ...) DARWIN(NSLog);
    src/dylibs/Foundation/misc.cpp:2263:void NSLog_(id fmt, ...) {
    src/dylibs/Foundation/misc.cpp:2264:    /* NSLog has one named integer operand; sending stringWithFormat:
    src/dylibs/Foundation/misc.cpp:2269:void NSLogv_(id fmt, std::uint64_t va) DARWIN(NSLogv);
    src/dylibs/Foundation/misc.cpp:2270:void NSLogv_(id fmt, std::uint64_t va) {
    src/dylibs/Foundation/misc.cpp:2332:int NSPointInRect_(double px, double py, double x, double y, double w, double h) DARWIN(NSPointInRect);
    src/dylibs/Foundation/misc.cpp:2333:int NSPointInRect_(double px, double py, double x, double y, double w, double h) {
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
    ✓ • 0ms

## Activity

    $ rg --files src modules | rg -i 'webkit|browser|webview' && command -v pkg-config && pkg-config --list-all | rg 'webkit|gtk|cef|qt6|Qt6'
    ✗ (1) • 0ms

## Activity

    $ coredumpctl --no-pager debug 223871 --debugger-arguments='-q -batch -x work/roblox-sign-in-core.gdb' > work/logs/roblox-sign-in-core.log 2>&1
    ✓ • 4.64s

## Activity

    $ tail -n 65 work/logs/roblox-sign-in-core.log && sed -n '2258,2278p' src/dylibs/Foundation/misc.cpp && sed -n '1,170p' src/dylibs/AppKit/stack_view.cpp
    Thread 3 (Thread 0x7fd6a77fe6c0 (LWP 224107)):
    #0  0x00007fd70b71e46d in syscall () from /usr/lib/libc.so.6
    #1  0x00007fd70b3fe4d6 in (anonymous namespace)::wait (operation=..., address=..., value=..., timeout=...) at /mnt/media/machoman/src/dylibs/libSystem/ulock.cpp:49
    #2  0x00007fd70b3fe1cc in darwin_ulock_wait (operation=..., address=..., value=..., timeout=...) at /mnt/media/machoman/src/dylibs/libSystem/ulock.cpp:61
    #3  0x000056090c140fa1 in mm_hostcall () at /mnt/media/machoman/src/runtime/hostcall_x86_64.S:69
    #4  0x000056090c11b1ad in (anonymous namespace)::guest_process::run_thread((anonymous namespace)::guest_thread&, unsigned long)::$_0::operator()(void const*) const (this=..., fn=...) at /mnt/media/machoman/src/runtime/loader.cpp:1741
    #5  0x000056090c11899c in (anonymous namespace)::guest_process::run_thread (this=..., t=..., pc=...) at /mnt/media/machoman/src/runtime/loader.cpp:1770
    #6  0x000056090c117507 in (anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0::operator()() const (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1985
    #7  0x000056090c1174c5 in std::__invoke_impl<void, (anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0>(std::__invoke_other, (anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0&&) (__f=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/invoke.h:63
    #8  0x000056090c117485 in std::__invoke<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0>((anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0&&) (__fn=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/invoke.h:98
    #9  0x000056090c11745d in std::thread::_Invoker<std::tuple<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0> >::_M_invoke<0ul> (this=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/std_thread.h:303
    #10 0x000056090c117435 in std::thread::_Invoker<std::tuple<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0> >::operator() (this=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/std_thread.h:310
    #11 0x000056090c117389 in std::thread::_State_impl<std::thread::_Invoker<std::tuple<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0> > >::_M_run (this=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/std_thread.h:255
    #12 0x00007fd70baea859 in ?? () from /usr/lib/libstdc++.so.6
    #13 0x00007fd70b698042 in ?? () from /usr/lib/libc.so.6
    #14 0x00007fd70b72072c in ?? () from /usr/lib/libc.so.6
    
    Thread 2 (Thread 0x7fd6c9bfe6c0 (LWP 224058)):
    #0  0x00007fd70b71e46d in syscall () from /usr/lib/libc.so.6
    #1  0x00007fd70b3fe4d6 in (anonymous namespace)::wait (operation=..., address=..., value=..., timeout=...) at /mnt/media/machoman/src/dylibs/libSystem/ulock.cpp:49
    #2  0x00007fd70b3fe1cc in darwin_ulock_wait (operation=..., address=..., value=..., timeout=...) at /mnt/media/machoman/src/dylibs/libSystem/ulock.cpp:61
    #3  0x000056090c140fa1 in mm_hostcall () at /mnt/media/machoman/src/runtime/hostcall_x86_64.S:69
    #4  0x000056090c11b1ad in (anonymous namespace)::guest_process::run_thread((anonymous namespace)::guest_thread&, unsigned long)::$_0::operator()(void const*) const (this=..., fn=...) at /mnt/media/machoman/src/runtime/loader.cpp:1741
    #5  0x000056090c11899c in (anonymous namespace)::guest_process::run_thread (this=..., t=..., pc=...) at /mnt/media/machoman/src/runtime/loader.cpp:1770
    #6  0x000056090c117507 in (anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0::operator()() const (this=...) at /mnt/media/machoman/src/runtime/loader.cpp:1985
    #7  0x000056090c1174c5 in std::__invoke_impl<void, (anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0>(std::__invoke_other, (anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0&&) (__f=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/invoke.h:63
    #8  0x000056090c117485 in std::__invoke<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0>((anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0&&) (__fn=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/invoke.h:98
    #9  0x000056090c11745d in std::thread::_Invoker<std::tuple<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0> >::_M_invoke<0ul> (this=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/std_thread.h:303
    #10 0x000056090c117435 in std::thread::_Invoker<std::tuple<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0> >::operator() (this=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/std_thread.h:310
    #11 0x000056090c117389 in std::thread::_State_impl<std::thread::_Invoker<std::tuple<(anonymous namespace)::guest_process::api_thread_create(unsigned long, unsigned long, unsigned long, unsigned long*)::$_0> > >::_M_run (this=...) at /usr/bin/../lib64/gcc/x86_64-pc-linux-gnu/16/../../../../include/c++/16/bits/std_thread.h:255
    #12 0x00007fd70baea859 in ?? () from /usr/lib/libstdc++.so.6
    #13 0x00007fd70b698042 in ?? () from /usr/lib/libc.so.6
    #14 0x00007fd70b72072c in ?? () from /usr/lib/libc.so.6
    
    Thread 1 (Thread 0x7fd70bd80840 (LWP 223871)):
    #0  0x00007fd70b69a11c in ?? () from /usr/lib/libc.so.6
    #1  0x00007fd70b63e5d0 in raise () from /usr/lib/libc.so.6
    #2  0x000056090c1051cc in (anonymous namespace)::on_fault (signum=..., info=..., uctx=...) at /mnt/media/machoman/src/runtime/loader.cpp:280
    #3  <signal handler called>
    #4  0x00007fd70b2216c2 in ak::(anonymous namespace)::set_window_recursively (view=..., window=...) at /mnt/media/machoman/src/dylibs/AppKit/views.cpp:200
    #5  0x00007fd70b2218db in ak::(anonymous namespace)::set_window_recursively (view=..., window=...) at /mnt/media/machoman/src/dylibs/AppKit/views.cpp:208
    #6  0x00007fd70b223aef in ak::(anonymous namespace)::view_addSubview (self=..., sub=...) at /mnt/media/machoman/src/dylibs/AppKit/views.cpp:217
    #7  0x00007fd70b223b8d in ak::(anonymous namespace)::view_addSubviewPositioned (self=..., sub=...) at /mnt/media/machoman/src/dylibs/AppKit/views.cpp:222
    #8  0x000056090c140fa1 in mm_hostcall () at /mnt/media/machoman/src/runtime/hostcall_x86_64.S:69
    #9  0x000056090c11b1ad in (anonymous namespace)::guest_process::run_thread((anonymous namespace)::guest_thread&, unsigned long)::$_0::operator()(void const*) const (this=..., fn=...) at /mnt/media/machoman/src/runtime/loader.cpp:1741
    #10 0x000056090c118a03 in (anonymous namespace)::guest_process::run_thread (this=..., t=..., pc=...) at /mnt/media/machoman/src/runtime/loader.cpp:1778
    #11 0x000056090c1128e2 in (anonymous namespace)::guest_process::api_call_guest (fn_va=..., args=..., nargs=...) at /mnt/media/machoman/src/runtime/loader.cpp:2175
    #12 0x00007fd70bd07ee0 in mm_objc_block_invoke (block=..., nargs=..., args=...) at /mnt/media/machoman/src/dylibs/libobjc/objc.cpp:1582
    #13 0x00007fd70b58d90c in fnd::(anonymous namespace)::run_item (item=...) at /mnt/media/machoman/src/dylibs/Foundation/dispatch.cpp:58
    #14 0x00007fd70b58d622 in fnd::dispatch_drain_main_queue () at /mnt/media/machoman/src/dylibs/Foundation/dispatch.cpp:148
    #15 0x00007fd70b56461a in fnd::(anonymous namespace)::run_loop_drain (l=...) at /mnt/media/machoman/src/dylibs/Foundation/misc.cpp:1365
    rip            0x7fd70b69a11c      0x7fd70b69a11c
    rsp            0x56092a0ef640      0x56092a0ef640
    rax            0x0                 0
    rdi            0x36a7f             223871
    rsi            0x36a7f             223871
    rdx            0xb                 11
    => 0x7fd70b69a11c:    mov    %eax,%r8d
       0x7fd70b69a11f:    neg    %r8d
       0x7fd70b69a122:    cmp    $0xfffff000,%eax
       0x7fd70b69a127:    mov    $0x0,%eax
       0x7fd70b69a12c:    cmova  %r8d,%eax
       0x7fd70b69a130:    jmp    0x7fd70b69a0a5
       0x7fd70b69a135:    nopl   (%rax)
       0x7fd70b69a138:    mov    %r9,%rdi
    int CFRunLoopIsWaiting_(id) DARWIN(CFRunLoopIsWaiting);
    int CFRunLoopIsWaiting_(id) { return 0; }
    
    /* NSLog and the Foundation free functions. */
    void NSLog_(id fmt, ...) DARWIN(NSLog);
    void NSLog_(id fmt, ...) {
        /* NSLog has one named integer operand; sending stringWithFormat:
         * would incorrectly skip self/selector/format in the Intel save area. */
        const auto text = format_call_arguments(fmt, 1);
        std::fprintf(stderr, "%s\n", text.c_str());
    }
    void NSLogv_(id fmt, std::uint64_t va) DARWIN(NSLogv);
    void NSLogv_(id fmt, std::uint64_t va) {
        const auto text = format_va_arguments(fmt, va);
        std::fprintf(stderr, "%s\n", text.c_str());
    }
    id NSStringFromClass_(Class c) DARWIN(NSStringFromClass);
    id NSStringFromClass_(Class c) { return c ? autorelease(string_from_utf8(mm_objc_class_name(c))) : nullptr; }
    Class NSClassFromString_(id name) DARWIN(NSClassFromString);
    Class NSClassFromString_(id name) { return mm_objc_class_named(to_utf8(name).c_str()); }
    id NSStringFromSelector_(SEL s) DARWIN(NSStringFromSelector);
    /* Stack containers must own the full NSView payload: their children and
     * window membership participate in the same hierarchy as ordinary views. */
    #include "appkit.h"
    #include <algorithm>
    
    MM_FW_CLASS(NSStackView);
    
    namespace ak {
    namespace {
    struct stack_object {
        view_object view;
        std::vector<id> arranged; // retained by NSView's subviews
        std::uint64_t orientation = 0, alignment = 0, distribution = 0;
        double spacing = 8;
    };
    stack_object *stack(id self) { return as<stack_object>(self); }
    std::uint64_t stack_views(id self, SEL) {
        id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
        for (id view : stack(self)->arranged) send(array, "addObject:", ret(view));
        return ret(array);
    }
    std::uint64_t stack_insert(id self, SEL, id view, std::uint64_t index) {
        if (!view || view == self || !is_kind(view, cls("NSView"))) return 0;
        auto &views = stack(self)->arranged;
        if (std::find(views.begin(), views.end(), view) != views.end()) return 0;
        send(self, "addSubview:", ret(view));
        views.insert(views.begin() + static_cast<std::ptrdiff_t>(std::min(index, std::uint64_t(views.size()))), view);
        return 0;
    }
    std::uint64_t stack_add(id self, SEL, id view) { return stack_insert(self, nullptr, view, stack(self)->arranged.size()); }
    std::uint64_t stack_remove(id self, SEL, id view) {
        auto &views = stack(self)->arranged;
        views.erase(std::remove(views.begin(), views.end(), view), views.end());
        if (view && obj(send(view, "superview")) == self) send(view, "removeFromSuperview");
        return 0;
    }
    std::uint64_t stack_factory(id klass, SEL, id views) {
        id self = obj(send(klass, "new"));
        const auto count = send(views, "count");
        for (std::uint64_t i = 0; i < count; ++i) stack_add(self, nullptr, obj(send(views, "objectAtIndex:", i)));
        return ret(autorelease(self));
    }
    }
    void register_stack_view() {
        Class c = mm_objc_define_class("NSStackView", cls("NSView"), sizeof(stack_object),
            MM_FW_CLASS_REF(NSStackView), MM_FW_META_REF(NSStackView));
        register_lifecycle<stack_object>(c);
        MM_CLASS_METHODS(c, {"stackViewWithViews:", MM_IMP(stack_factory), "@24@0:8@16"});
        MM_METHODS(c,
            {"views", MM_IMP(stack_views), "@16@0:8"}, {"arrangedSubviews", MM_IMP(stack_views), "@16@0:8"},
            {"addArrangedSubview:", MM_IMP(stack_add), "v24@0:8@16"},
            {"insertArrangedSubview:atIndex:", MM_IMP(stack_insert), "v32@0:8@16Q24"},
            {"removeArrangedSubview:", MM_IMP(stack_remove), "v24@0:8@16"},
            {"addView:inGravity:", MM_IMP(+[](id s, SEL, id view, std::uint64_t) { return stack_add(s, nullptr, view); }), "v32@0:8@16q24"},
            {"removeView:", MM_IMP(stack_remove), "v24@0:8@16"},
            {"orientation", MM_IMP(+[](id s, SEL) { return stack(s)->orientation; }), "q16@0:8"},
            {"setOrientation:", MM_IMP(+[](id s, SEL, std::uint64_t v) -> std::uint64_t { stack(s)->orientation = v; return 0; }), "v24@0:8q16"},
            {"alignment", MM_IMP(+[](id s, SEL) { return stack(s)->alignment; }), "q16@0:8"},
            {"setAlignment:", MM_IMP(+[](id s, SEL, std::uint64_t v) -> std::uint64_t { stack(s)->alignment = v; return 0; }), "v24@0:8q16"},
            {"distribution", MM_IMP(+[](id s, SEL) { return stack(s)->distribution; }), "q16@0:8"},
            {"setDistribution:", MM_IMP(+[](id s, SEL, std::uint64_t v) -> std::uint64_t { stack(s)->distribution = v; return 0; }), "v24@0:8q16"},
            {"spacing", MM_IMP(+[](id s, SEL) { return stack(s)->spacing; }), "d16@0:8"},
            {"setSpacing:", MM_IMP(+[](id s, SEL, double v) -> std::uint64_t { stack(s)->spacing = v; return 0; }), "v24@0:8d16"});
    }
    }
    ✓ • 0ms

## Activity

    $ pkg-config --list-all | rg -i 'webkit|gtk|cef|qt6|qt5' ; rg --files /usr/lib | rg 'lib(webkit|gtk|cef|Qt[56]Web)' | head -n 30
    gdk-3.0                        GDK - GTK+ Drawing Kit
    gdk-broadway-3.0               GDK - GTK+ Drawing Kit
    gdk-wayland-3.0                GDK - GTK+ Drawing Kit
    gdk-x11-3.0                    GDK - GTK+ Drawing Kit
    gtk+-3.0                       GTK+ - GTK+ Graphical UI Library
    gtk+-broadway-3.0              GTK+ - GTK+ Graphical UI Library
    gtk+-unix-print-3.0            GTK+ - GTK+ Unix print support
    gtk+-wayland-3.0               GTK+ - GTK+ Graphical UI Library
    gtk+-x11-3.0                   GTK+ - GTK+ Graphical UI Library
    dbusmenu-gtk3-0.4              libdbusmenu-gtk3 - libdbusmenu-gtk3.
    Qt6LabsAnimation               Qt6 LabsAnimation - Qt LabsAnimation module
    Qt6LabsFolderListModel         Qt6 LabsFolderListModel - Qt LabsFolderListModel module
    Qt6LabsPlatform                Qt6 LabsPlatform - Qt LabsPlatform module
    Qt6LabsQmlModels               Qt6 LabsQmlModels - Qt LabsQmlModels module
    Qt6LabsSettings                Qt6 LabsSettings - Qt LabsSettings module
    Qt6LabsSharedImage             Qt6 LabsSharedImage - Qt LabsSharedImage module
    Qt6LabsStyleKit                Qt6 LabsStyleKit - Qt LabsStyleKit module
    Qt6LabsStyleKitImpl            Qt6 LabsStyleKitImpl - Qt LabsStyleKitImpl module
    Qt6LabsSynchronizer            Qt6 LabsSynchronizer - Qt LabsSynchronizer module
    Qt6LabsWavefrontMesh           Qt6 LabsWavefrontMesh - Qt LabsWavefrontMesh module
    Qt6Qml                         Qt6 Qml - Qt Qml module
    Qt6QmlCompiler                 Qt6 QmlCompiler - Qt QmlCompiler module
    Qt6QmlCore                     Qt6 QmlCore - Qt QmlCore module
    Qt6QmlDesignSupport            Qt6 QmlDesignSupport - Qt QmlDesignSupport module
    Qt6QmlIntegration              Qt6 QmlIntegration - Qt QmlIntegration module
    Qt6QmlLocalStorage             Qt6 QmlLocalStorage - Qt QmlLocalStorage module
    Qt6QmlMeta                     Qt6 QmlMeta - Qt QmlMeta module
    Qt6QmlModels                   Qt6 QmlModels - Qt QmlModels module
    Qt6QmlNetwork                  Qt6 QmlNetwork - Qt QmlNetwork module
    Qt6QmlWorkerScript             Qt6 QmlWorkerScript - Qt QmlWorkerScript module
    Qt6QmlXmlListModel             Qt6 QmlXmlListModel - Qt QmlXmlListModel module
    Qt6Quick                       Qt6 Quick - Qt Quick module
    Qt6QuickControls2              Qt6 QuickControls2 - Qt QuickControls2 module
    Qt6QuickControls2Basic         Qt6 QuickControls2Basic - Qt QuickControls2Basic module
    Qt6QuickControls2BasicStyleImpl Qt6 QuickControls2BasicStyleImpl - Qt QuickControls2BasicStyleImpl module
    Qt6QuickControls2FluentWinUI3  Qt6 QuickControls2FluentWinUI3 - Qt QuickControls2FluentWinUI3 module
    Qt6QuickControls2FluentWinUI3StyleImpl Qt6 QuickControls2FluentWinUI3StyleImpl - Qt QuickControls2FluentWinUI3StyleImpl module
    Qt6QuickControls2Fusion        Qt6 QuickControls2Fusion - Qt QuickControls2Fusion module
    Qt6QuickControls2FusionStyleImpl Qt6 QuickControls2FusionStyleImpl - Qt QuickControls2FusionStyleImpl module
    Qt6QuickControls2Imagine       Qt6 QuickControls2Imagine - Qt QuickControls2Imagine module
    Qt6QuickControls2ImagineStyleImpl Qt6 QuickControls2ImagineStyleImpl - Qt QuickControls2ImagineStyleImpl module
    Qt6QuickControls2Impl          Qt6 QuickControls2Impl - Qt QuickControls2Impl module
    Qt6QuickControls2Material      Qt6 QuickControls2Material - Qt QuickControls2Material module
    Qt6QuickControls2MaterialStyleImpl Qt6 QuickControls2MaterialStyleImpl - Qt QuickControls2MaterialStyleImpl module
    Qt6QuickControls2Native        Qt6 QuickControls2Native - Qt QuickControls2Native module
    Qt6QuickControls2Universal     Qt6 QuickControls2Universal - Qt QuickControls2Universal module
    Qt6QuickControls2UniversalStyleImpl Qt6 QuickControls2UniversalStyleImpl - Qt QuickControls2UniversalStyleImpl module
    Qt6QuickDialogs2               Qt6 QuickDialogs2 - Qt QuickDialogs2 module
    Qt6QuickDialogs2QuickImpl      Qt6 QuickDialogs2QuickImpl - Qt QuickDialogs2QuickImpl module
    Qt6QuickDialogs2Utils          Qt6 QuickDialogs2Utils - Qt QuickDialogs2Utils module
    Qt6QuickEffects                Qt6 QuickEffects - Qt QuickEffects module
    Qt6QuickLayouts                Qt6 QuickLayouts - Qt QuickLayouts module
    Qt6QuickParticles              Qt6 QuickParticles - Qt QuickParticles module
    Qt6QuickShapes                 Qt6 QuickShapes - Qt QuickShapes module
    Qt6QuickShapesDesignHelpers    Qt6 QuickShapesDesignHelpers - Qt QuickShapesDesignHelpers module
    Qt6QuickTemplates2             Qt6 QuickTemplates2 - Qt QuickTemplates2 module
    Qt6QuickTest                   Qt6 QuickTest - Qt QuickTest module
    Qt6QuickVectorImage            Qt6 QuickVectorImage - Qt QuickVectorImage module
    Qt6QuickVectorImageHelpers     Qt6 QuickVectorImageHelpers - Qt QuickVectorImageHelpers module
    Qt6QuickWidgets                Qt6 QuickWidgets - Qt QuickWidgets module
    Qt6Svg                         Qt6 Svg - Qt Svg module
    Qt6SvgWidgets                  Qt6 SvgWidgets - Qt SvgWidgets module
    libcanberra-gtk3               libcanberra-gtk3 - Gtk3 Event Sound API
    Qt6ShaderTools                 Qt6 ShaderTools - Qt ShaderTools module
    Qt6Core5Compat                 Qt6 Core5Compat - Qt Core5Compat module
    Qt6Designer                    Qt6 Designer - Qt Designer module
    Qt6Help                        Qt6 Help - Qt Help module
    Qt6Linguist                    Qt6 Linguist - Qt Linguist module
    Qt6UiPlugin                    Qt6 UiPlugin - Qt UiPlugin module
    Qt6UiTools                     Qt6 UiTools - Qt UiTools module
    Qt6Multimedia                  Qt6 Multimedia - Qt Multimedia module
    Qt6MultimediaQuick             Qt6 MultimediaQuick - Qt MultimediaQuick module
    Qt6MultimediaWidgets           Qt6 MultimediaWidgets - Qt MultimediaWidgets module
    Qt6Quick3DSpatialAudio         Qt6 Quick3DSpatialAudio - Qt Quick3DSpatialAudio module
    Qt6SpatialAudio                Qt6 SpatialAudio - Qt SpatialAudio module
    Qt6Positioning                 Qt6 Positioning - Qt Positioning module
    Qt6PositioningQuick            Qt6 PositioningQuick - Qt PositioningQuick module
    Qt6TextToSpeech                Qt6 TextToSpeech - Qt TextToSpeech module
    Qt6HunspellInputMethod         Qt6 HunspellInputMethod - Qt HunspellInputMethod module
    Qt6VirtualKeyboard             Qt6 VirtualKeyboard - Qt VirtualKeyboard module
    Qt6VirtualKeyboardQml          Qt6 VirtualKeyboardQml - Qt VirtualKeyboardQml module
    Qt6VirtualKeyboardSettings     Qt6 VirtualKeyboardSettings - Qt VirtualKeyboardSettings module
    Qt6QuickTimeline               Qt6 QuickTimeline - Qt QuickTimeline module
    Qt6QuickTimelineBlendTrees     Qt6 QuickTimelineBlendTrees - Qt QuickTimelineBlendTrees module
    ayatana-appindicator3-0.1      ayatana-appindicator3-0.1 - Ayatana Application Indicators (for GTK-3+)
    gtk4-atspi                     GTK - GTK AT-SPI accessibility support
    gtk4-broadway                  GTK - GTK Graphical UI Library
    gtk4-unix-print                GTK - GTK Unix print support
    gtk4-wayland                   GTK - GTK Graphical UI Library
    gtk4-x11                       GTK - GTK Graphical UI Library
    gtk4                           GTK - GTK Graphical UI Library
    javascriptcoregtk-6.0          JavaScriptCoreGTK+ - GTK+ version of the JavaScriptCore engine
    webkitgtk-6.0                  WebKitGTK - Web content engine for GTK
    webkitgtk-web-process-extension-6.0 WebKitGTK web process extensions - Web content engine for GTK - web process extensions
    gtk-layer-shell-0              gtk-layer-shell - Use the Layer Shell Wayland protocol with GTK
    Qt6WaylandCompositor           Qt6 WaylandCompositor - Qt WaylandCompositor module
    Qt6WaylandCompositorIviapplication Qt6 WaylandCompositorIviapplication - Qt WaylandCompositorIviapplication module
    Qt6WaylandCompositorPresentationTime Qt6 WaylandCompositorPresentationTime - Qt WaylandCompositorPresentationTime module
    Qt6WaylandCompositorWLShell    Qt6 WaylandCompositorWLShell - Qt WaylandCompositorWLShell module
    Qt6WaylandCompositorXdgShell   Qt6 WaylandCompositorXdgShell - Qt WaylandCompositorXdgShell module
    libportal-gtk4                 libportal-gtk4 - Portal API wrappers (GTK4)
    gtkmm-3.0                      gtkmm - C++ binding for the GTK+ toolkit
    gtksourceview-4                gtksourceview-4 - Source code editing Gtk+ widget.
    gtksourceviewmm-4.0            gtksourceviewmm - C++ binding for gtksourceview
    Qt6Sensors                     Qt6 Sensors - Qt Sensors module
    Qt6SensorsQuick                Qt6 SensorsQuick - Qt SensorsQuick module
    poppler-qt6                    poppler-qt6 - Qt6 bindings for poppler
    accounts-qt6                   libaccounts-qt6 - Accounts Library
    libsignon-qt6                  libsignon-qt6 - Client library for the Single Sign On daemon, Qt6 bindings.
    Qt6WebChannel                  Qt6 WebChannel - Qt WebChannel module
    Qt6WebChannelQuick             Qt6 WebChannelQuick - Qt WebChannelQuick module
    Qt6Pdf                         Qt6 Pdf - Qt Pdf module
    Qt6PdfQuick                    Qt6 PdfQuick - Qt PdfQuick module
    Qt6PdfWidgets                  Qt6 PdfWidgets - Qt PdfWidgets module
    Qt6WebEngineCore               Qt6 WebEngineCore - Qt WebEngineCore module
    Qt6WebEngineQuick              Qt6 WebEngineQuick - Qt WebEngineQuick module
    Qt6WebEngineQuickDelegatesQml  Qt6 WebEngineQuickDelegatesQml - Qt WebEngineQuickDelegatesQml module
    Qt6WebEngineWidgets            Qt6 WebEngineWidgets - Qt WebEngineWidgets module
    polkit-qt6-1                   polkit-qt6-1 - Convenience library for using polkit with a Qt-styled API
    polkit-qt6-agent-1             polkit-qt6-agent-1 - Convenience library for using polkit Agent with a Qt-styled API
    polkit-qt6-core-1              polkit-qt6-core-1 - Convenience library for using polkit with a Qt-styled API, non-GUI classes
    polkit-qt6-gui-1               polkit-qt6-gui-1 - Convenience library for using polkit with a Qt-styled API, GUI classes
    Qt6Location                    Qt6 Location - Qt Location module
    Qt6Concurrent                  Qt6 Concurrent - Qt Concurrent module
    Qt6Core                        Qt6 Core - Qt Core module
    Qt6DBus                        Qt6 DBus - Qt DBus module
    Qt6Gui                         Qt6 Gui - Qt Gui module
    Qt6Network                     Qt6 Network - Qt Network module
    Qt6OpenGL                      Qt6 OpenGL - Qt OpenGL module
    Qt6OpenGLWidgets               Qt6 OpenGLWidgets - Qt OpenGLWidgets module
    Qt6Platform                    Qt6 Platform - Qt Platform module
    Qt6PrintSupport                Qt6 PrintSupport - Qt PrintSupport module
    Qt6Sql                         Qt6 Sql - Qt Sql module
    Qt6Test                        Qt6 Test - Qt Test module
    Qt6WaylandClient               Qt6 WaylandClient - Qt WaylandClient module
    Qt6Widgets                     Qt6 Widgets - Qt Widgets module
    Qt6Xml                         Qt6 Xml - Qt Xml module
    Qt5Concurrent                  Qt5 Concurrent - Qt Concurrent module
    Qt5Core                        Qt5 Core - Qt Core module
    Qt5DBus                        Qt5 DBus - Qt DBus module
    Qt5Gui                         Qt5 Gui - Qt Gui module
    Qt5Network                     Qt5 Network - Qt Network module
    Qt5OpenGL                      Qt5 OpenGL - Qt OpenGL module
    Qt5OpenGLExtensions            Qt5 OpenGLExtensions - Qt OpenGLExtensions module
    Qt5PrintSupport                Qt5 PrintSupport - Qt PrintSupport module
    Qt5Sql                         Qt5 Sql - Qt Sql module
    Qt5Test                        Qt5 Test - Qt Unit Testing Library
    Qt5Widgets                     Qt5 Widgets - Qt Widgets module
    Qt5Xml                         Qt5 Xml - Qt Xml module
    /usr/lib/libQt6WebEngineWidgets.so.6.12.0
    /usr/lib/libQt6WebEngineWidgets.prl
    /usr/lib/libQt6WebEngineQuickDelegatesQml.so.6.12.0
    /usr/lib/libQt6WebEngineQuickDelegatesQml.prl
    /usr/lib/libQt6WebEngineQuick.so.6.12.0
    /usr/lib/libQt6WebEngineQuick.prl
    /usr/lib/libQt6WebEngineCore.so.6.12.0
    /usr/lib/libQt6WebEngineCore.prl
    /usr/lib/libQt6WebChannelQuick.so.6.12.0
    /usr/lib/libQt6WebChannelQuick.prl
    /usr/lib/libQt6WebChannel.so.6.12.0
    /usr/lib/libQt6WebChannel.prl
    /usr/lib/libgtksourceviewmm-4.0.so.0.0.0
    /usr/lib/libgtksourceview-4.so.0.0.0
    /usr/lib/libgtk-3.so.0.2420.32
    /usr/lib/libgtk-4.so.1.2400.1
    /usr/lib/webkitgtk-6.0/injected-bundle/libwebkitgtkinjectedbundle.so
    /usr/lib/libwebkitgtk-6.0.so.4.19.5
    /usr/lib/libgtk-layer-shell.so.0.10.1
    /usr/lib/libgtkmm-3.0.so.1.1.0
    ✓ • 55ms

## Activity

    $ sed -n '1,155p' modules/MEVK/src/metal/CMakeLists.txt && sed -n '1,105p' CMakeLists.txt && sed -n '1,215p' include/machoman/objc_framework.h && sed -n '175,222p' src/dylibs/AppKit/views.cpp
    add_library(MEVK_Metal SHARED metal.cpp render_metal.cpp)
    target_include_directories(MEVK_Metal PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(MEVK_Metal PRIVATE MEVK::mevk libobjc Foundation)
    target_compile_options(MEVK_Metal PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -fvisibility=hidden)
    set(MEVK_METAL_DIR "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Metal.framework/Versions/A")
    # An earlier build may have installed a symlink to the stub at this output.
    # Remove only that generated symlink so the linker cannot overwrite its target.
    if(IS_SYMLINK "${MEVK_METAL_DIR}/Metal.so")
        file(REMOVE "${MEVK_METAL_DIR}/Metal.so")
    endif()
    set_target_properties(MEVK_Metal PROPERTIES
        PREFIX "" OUTPUT_NAME "Metal" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${MEVK_METAL_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A"
        INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A")
    
    # A real CoreGraphics export is required: the loader asks the framework
    # named by the bind for a synthesized stub before searching other libraries.
    add_library(MEVK_CoreGraphics SHARED coregraphics_metal.cpp
        "${PROJECT_SOURCE_DIR}/src/dylibs/symbol-synthesis/synthesise.cpp")
    target_include_directories(MEVK_CoreGraphics PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src/dylibs/symbol-synthesis")
    target_link_libraries(MEVK_CoreGraphics PRIVATE MEVK_Metal libobjc AppKit)
    target_compile_options(MEVK_CoreGraphics PRIVATE -Wall -Wextra -fvisibility=hidden)
    set(MEVK_COREGRAPHICS_DIR "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreGraphics.framework/Versions/A")
    if(IS_SYMLINK "${MEVK_COREGRAPHICS_DIR}/CoreGraphics.so")
        file(REMOVE "${MEVK_COREGRAPHICS_DIR}/CoreGraphics.so")
    endif()
    set_target_properties(MEVK_CoreGraphics PROPERTIES
        PREFIX "" OUTPUT_NAME "CoreGraphics" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${MEVK_COREGRAPHICS_DIR}"
        BUILD_RPATH "${MEVK_METAL_DIR};${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/AppKit.framework/Versions/C"
        INSTALL_RPATH "${MEVK_METAL_DIR};${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/AppKit.framework/Versions/C")
    
    if(MEVK_BUILD_TESTS)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../tests/metal" "${CMAKE_CURRENT_BINARY_DIR}/tests")
    endif()
    cmake_minimum_required(VERSION 3.25)
    project(machoman CXX C ASM)
    
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    
    add_library(machoman_core STATIC
        src/common/macho.cpp
        src/common/json.cpp
        src/venv/venv.cpp
        src/venv/install.cpp
        src/translator/arm64_decoder.cpp
        src/translator/x86_64_encoder.cpp
        src/translator/translator.cpp
        src/translator/translator_simd.cpp
        src/translator/translator_simd_helpers.cpp
        src/translator/translator_simd_crypto.cpp
        src/translator/translator_simd_fp_helpers.cpp
        src/translator/translator_simd_half.cpp
        src/translator/translator_fp.cpp
        src/runtime/loader.cpp
        src/runtime/environment.cpp
        src/runtime/translation_cache.cpp
        src/runtime/hostcall_x86_64.S
        src/runtime/native_x86_64.S
        src/runtime/syscall_table.cpp
    )
    target_include_directories(machoman_core PUBLIC include)
    set_target_properties(machoman_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    
    if(MSVC)
        target_compile_options(machoman_core PRIVATE /W4)
    else()
        target_compile_options(machoman_core PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
    endif()
    
    add_executable(machoman src/cli/main.cpp)
    target_link_libraries(machoman PRIVATE machoman_core)
    find_package(Threads REQUIRED)
    target_link_libraries(machoman_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    
    if(NOT WIN32)
        add_library(machoman_ipc STATIC src/ipc/kernel.cpp src/ipc/transport.cpp)
        target_include_directories(machoman_ipc PUBLIC include)
        target_link_libraries(machoman_ipc PUBLIC Threads::Threads)
        target_compile_options(machoman_ipc PRIVATE -Wall -Wextra -Wconversion)
        set_target_properties(machoman_ipc PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_libraries(machoman PRIVATE machoman_ipc)
    endif()
    
    enable_testing()
    
    add_library(machoman_audio_engine STATIC src/dylibs/audio/engine.cpp
        src/dylibs/audio/format.cpp src/dylibs/audio/backend_null.cpp)
    target_include_directories(machoman_audio_engine PUBLIC src/dylibs/audio)
    target_link_libraries(machoman_audio_engine PUBLIC Threads::Threads)
    set_target_properties(machoman_audio_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(machoman_audio_engine PRIVATE /W4)
    else()
        target_compile_options(machoman_audio_engine PRIVATE -Wall -Wextra)
    endif()
    
    # MEVK is developed separately from the CPU translator and host frameworks.
    # Enabling it also replaces the Metal framework stub on supported hosts.
    option(MACHOMAN_BUILD_MEVK "Build the experimental Metal-to-Vulkan module" OFF)
    if(MACHOMAN_BUILD_MEVK)
        add_subdirectory(modules/MEVK)
    endif()
    
    option(MACHOMAN_BUILD_EVILCOCOA "Build the OpenGL Cocoa control renderer" ON)
    if(MACHOMAN_BUILD_EVILCOCOA AND NOT WIN32)
        add_subdirectory(modules/evilcocoa)
    endif()
    
    # Host dylibs: real shared objects the runtime dlopen()s in place of Darwin
    # libraries, laid out like a macOS root under build/dylibs/.
    if(NOT WIN32)
        if(EXISTS "${CMAKE_SOURCE_DIR}/modules/libresolv/ns_name.c")
            add_subdirectory(src/dylibs/libresolv)
        endif()
        add_library(libSystem SHARED src/dylibs/libSystem/libSystem.cpp src/dylibs/libSystem/filesystem.cpp src/dylibs/libSystem/stdio.cpp src/dylibs/libSystem/xlocale.cpp src/dylibs/libSystem/callbacks.cpp src/dylibs/libSystem/kqueue.cpp src/dylibs/libSystem/commoncrypto.cpp src/dylibs/libSystem/malloc_zone.cpp src/dylibs/libSystem/socket.cpp)
        target_link_libraries(libSystem PRIVATE machoman_core machoman_ipc)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/ulock.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/xattr.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/uuid.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/passwd.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/sysinfo.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/proc_info.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/mach_ports.cpp src/dylibs/libSystem/mach_tasks.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/mach_messages.cpp src/dylibs/libSystem/mach_bootstrap.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/mach_memory.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/mach_host.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/processes.cpp src/dylibs/libSystem/shared_memory.cpp)
        target_sources(libSystem PRIVATE src/dylibs/libSystem/environment.cpp)
        target_compile_options(libSystem PRIVATE -Wall -Wextra -fvisibility=hidden)
        set_target_properties(libSystem PROPERTIES
            PREFIX "" OUTPUT_NAME "libSystem.B.dylib" SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/usr/lib")
    /* Helpers shared by the host frameworks (Foundation, AppKit, ...) that
     * define Objective-C classes on machoman's libobjc.
     *
     * A host class keeps its state in a C++ struct whose first member is the
     * objc header; `+alloc` placement-news it and `-dealloc` destroys it. Host
     * method implementations receive the guest's registers verbatim: integer
     * arguments in order, doubles in order, structs flattened (a method taking
     * an NSRect takes four doubles). See objc_host.h for the return rules. */
    #ifndef MACHOMAN_OBJC_FRAMEWORK_H
    #define MACHOMAN_OBJC_FRAMEWORK_H
    
    #include "machoman/objc_host.h"
    
    #include <cstddef>
    #include <cstdint>
    #include <new>
    
    #define MM_FW_EXPORT __attribute__((visibility("default")))
    /* An IMP from a plain function or a captureless lambda. */
    #define MM_IMP(fn) reinterpret_cast<void *>(+(fn))
    /* Class storage the guest binds to. Must be at namespace scope: inside an
     * anonymous namespace it would get internal linkage and the bind would
     * silently trap. */
    #define MM_FW_CLASS(name) MM_OBJC_CLASS_STORAGE(name)
    #define MM_FW_CLASS_REF(name) MM_OBJC_CLASS_REF(name)
    #define MM_FW_META_REF(name) MM_OBJC_METACLASS_REF(name)
    
    namespace mmfw {
    
    using id = mm_id;
    using Class = mm_objc_class *;
    using SEL = mm_SEL;
    
    /* ---- objects ------------------------------------------------------------ */
    
    /* Constructs a payload in place and restores the isa the allocation set.
     * The objc header is the first thing in the payload, directly or through
     * a base payload (a control is a view is a responder), so it is reached
     * through the object pointer rather than by naming a member. */
    template <typename T> T *construct(id o) {
        Class isa = o->isa;
        T *t = new (static_cast<void *>(o)) T;
        reinterpret_cast<mm_objc_object *>(t)->isa = isa;
        if (!mm_objc_construct_instance(o)) {
            t->~T();
            mm_objc_free_instance(o);
            return nullptr;
        }
        return t;
    }
    template <typename T> T *make(Class cls) { return construct<T>(mm_objc_alloc_raw(cls, 0)); }
    template <typename T> T *as(id o) { return reinterpret_cast<T *>(o); }
    
    inline std::uint64_t ret(const void *p) { return reinterpret_cast<std::uintptr_t>(p); }
    inline id obj(std::uint64_t v) { return reinterpret_cast<id>(v); }
    
    template <typename T> std::uint64_t alloc_imp(id cls, SEL) { return ret(make<T>(reinterpret_cast<Class>(cls))); }
    template <typename T> std::uint64_t dealloc_imp(id self, SEL) {
        mm_objc_destruct_instance(self);
        reinterpret_cast<T *>(self)->~T();
        mm_objc_free_instance(self);
        return 0;
    }
    
    /* +alloc/-dealloc that construct and destroy the C++ payload. */
    template <typename T> void register_lifecycle(Class cls) {
        mm_objc_add_class_method(cls, "alloc", MM_IMP(alloc_imp<T>), "@16@0:8");
        mm_objc_add_class_method(cls, "allocWithZone:", MM_IMP(alloc_imp<T>), "@24@0:8^v16");
        mm_objc_add_method(cls, "dealloc", MM_IMP(dealloc_imp<T>), "v16@0:8");
    }
    
    /* A class cluster: an abstract class (NSString, NSMutableArray) allocates
     * an instance of the concrete class that holds the payload. */
    template <typename T> struct cluster {
        static inline Class concrete = nullptr;
        static std::uint64_t alloc(id, SEL) { return ret(make<T>(concrete)); }
        static std::uint64_t alloc_mutable(id, SEL) {
            T *o = make<T>(concrete);
            if constexpr (requires(T *t) { t->mutable_ = true; }) o->mutable_ = true;
            return ret(o);
        }
    };
    template <typename T> void register_cluster(Class abstract, Class concrete, bool mutable_) {
        cluster<T>::concrete = concrete;
        void *imp = mutable_ ? MM_IMP(cluster<T>::alloc_mutable) : MM_IMP(cluster<T>::alloc);
        mm_objc_add_class_method(abstract, "alloc", imp, "@16@0:8");
        mm_objc_add_class_method(abstract, "allocWithZone:", imp, "@24@0:8^v16");
    }
    
    /* ---- method tables ------------------------------------------------------- */
    
    struct method { const char *sel; void *imp; const char *types; };
    inline void add_methods(Class cls, const method *m, std::size_t n) {
        for (std::size_t i = 0; i < n; i++) mm_objc_add_method(cls, m[i].sel, m[i].imp, m[i].types);
    }
    inline void add_class_methods(Class cls, const method *m, std::size_t n) {
        for (std::size_t i = 0; i < n; i++) mm_objc_add_class_method(cls, m[i].sel, m[i].imp, m[i].types);
    }
    #define MM_METHODS(cls, ...)                                                                                           \
        do {                                                                                                               \
            static const mmfw::method mm_tbl[] = {__VA_ARGS__};                                                            \
            mmfw::add_methods(cls, mm_tbl, sizeof mm_tbl / sizeof mm_tbl[0]);                                              \
        } while (0)
    #define MM_CLASS_METHODS(cls, ...)                                                                                     \
        do {                                                                                                               \
            static const mmfw::method mm_tbl[] = {__VA_ARGS__};                                                            \
            mmfw::add_class_methods(cls, mm_tbl, sizeof mm_tbl / sizeof mm_tbl[0]);                                        \
        } while (0)
    
    /* ---- messaging ----------------------------------------------------------- */
    
    inline std::uint64_t send(id r, const char *sel) { return mm_objc_send(r, mm_objc_sel(sel), 0, nullptr); }
    inline std::uint64_t send(id r, const char *sel, std::uint64_t a) { return mm_objc_send(r, mm_objc_sel(sel), 1, &a); }
    inline std::uint64_t send(id r, const char *sel, std::uint64_t a, std::uint64_t b) {
        const std::uint64_t v[2] = {a, b};
        return mm_objc_send(r, mm_objc_sel(sel), 2, v);
    }
    inline std::uint64_t send(id r, const char *sel, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        const std::uint64_t v[3] = {a, b, c};
        return mm_objc_send(r, mm_objc_sel(sel), 3, v);
    }
    inline bool responds(id r, const char *sel) { return r && send(r, "respondsToSelector:", ret(mm_objc_sel(sel))) != 0; }
    inline id retain(id o) { return mm_objc_retain(o); }
    inline void release(id o) { mm_objc_release(o); }
    inline id autorelease(id o) { return mm_objc_autorelease(o); }
    inline bool is_kind(id o, Class c) { return mm_objc_is_kind_of(o, c) != 0; }
    inline Class cls(const char *name) { return mm_objc_class_named(name); }
    
    /* Event-loop turns and dispatch work items own their temporary objects.
     * An outer guest pool may remain active for the lifetime of the app. */
    class autorelease_pool {
        id pool_;
    public:
        autorelease_pool() : pool_(obj(send(reinterpret_cast<id>(cls("NSAutoreleasePool")), "new"))) {}
        ~autorelease_pool() { if (pool_) send(pool_, "drain"); }
        autorelease_pool(const autorelease_pool &) = delete;
        autorelease_pool &operator=(const autorelease_pool &) = delete;
    };
    
    } // namespace mmfw
    
    #endif
        if (o->posts_bounds_notifications) post_view_notification(self, "NSViewBoundsDidChangeNotification");
        return 0;
    }
    std::uint64_t view_postsFrameChangedNotifications(id self, SEL) { return v(self)->posts_frame_notifications; }
    std::uint64_t view_setPostsFrameChangedNotifications(id self, SEL, std::uint64_t value) {
        v(self)->posts_frame_notifications = (value & 0xff) != 0;
        return 0;
    }
    std::uint64_t view_postsBoundsChangedNotifications(id self, SEL) { return v(self)->posts_bounds_notifications; }
    std::uint64_t view_setPostsBoundsChangedNotifications(id self, SEL, std::uint64_t value) {
        v(self)->posts_bounds_notifications = (value & 0xff) != 0;
        return 0;
    }
    
    std::uint64_t view_superview(id self, SEL) { return ret(v(self)->superview); }
    std::uint64_t view_window(id self, SEL) {
        return ret(v(self)->window);
    }
    std::uint64_t view_subviews(id self, SEL) {
        id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
        for (id sub : v(self)->subviews) send(array, "addObject:", ret(sub));
        return ret(array);
    }
    /* A view's window is inherited by everything under it. */
    void set_window_recursively(id view, id window) {
        id previous = v(view)->window;
        if (previous != window) send(view, "willChangeValueForKey:", ret(string("window")));
        if (previous && previous != window && w(previous)->first_responder == view) {
            /* A detached view cannot remain a window's weak first responder,
             * even if it refuses resignation while being removed. */
            w(previous)->first_responder = nullptr;
        }
        v(view)->window = window;
        for (id sub : v(view)->subviews) set_window_recursively(sub, window);
        if (previous != window) send(view, "didChangeValueForKey:", ret(string("window")));
    }
    std::uint64_t view_addSubview(id self, SEL, id sub) {
        if (!sub) return 0;
        view_object *o = v(self);
        if (std::find(o->subviews.begin(), o->subviews.end(), sub) != o->subviews.end()) return 0;
        o->subviews.push_back(retain(sub));
        v(sub)->superview = self;
        set_window_recursively(sub, o->window);
        send(sub, "setNextResponder:", ret(self));
        send(sub, "viewDidMoveToWindow");
        return 0;
    }
    std::uint64_t view_addSubviewPositioned(id self, SEL, id sub, std::uint64_t, id) { return view_addSubview(self, nullptr, sub); }
    ✓ • 0ms

## Activity

    $ sed -n '1,165p' src/dylibs/AppKit/appkit.h && rg -n 'register_lifecycle|register_.*|dispatch.*main|performSelector.*main|run_loop' src/dylibs/AppKit/appkit.h src/dylibs/Foundation/foundation.h include/machoman/runtime.h include/machoman/objc_host.h | head -n 70 && rg -n 'WKWeb|WKUser|WKNavigation|WKHTTPCookie|WebView' work/roblox-import-audit.tsv | head -n 25 && pkg-config --modversion Qt6WebEngineWidgets
    /* Internal header of the host AppKit.
     *
     * The object model is real AppKit: NSResponder -> NSView/NSWindow/
     * NSApplication, geometry in points with the origin at the bottom left,
     * `-drawRect:` and the responder chain. The platform underneath is
     * Wayland: one xdg-toplevel per NSWindow, EGL for the OpenGL contexts the
     * app draws with, wl_seat for events.
     *
     * Only the Wayland backend (`platform.cpp`) talks to the compositor; the
     * AppKit classes call it through the `platform` interface below, so the
     * object model stays testable without a display. */
    #ifndef MACHOMAN_APPKIT_H
    #define MACHOMAN_APPKIT_H
    
    #include "machoman/objc_framework.h"
    #include "machoman/runtime.h"
    #include "clipboard.h"
    
    #include <cstdint>
    #include <cstring>
    #include <map>
    #include <set>
    #include <string>
    #include <vector>
    #ifdef MM_APPKIT_EVILCOCOA
    #include <evilcocoa/renderer.h>
    #endif
    #ifdef MM_APPKIT_MEVK
    #include <memory>
    namespace mevk { class Texture; }
    #endif
    
    #define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)
    
    namespace ak {
    
    using namespace mmfw;
    
    struct rect { double x = 0, y = 0, w = 0, h = 0; };
    struct point { double x = 0, y = 0; };
    struct size { double w = 0, h = 0; };
    
    /* ---- the platform window a guest NSWindow is shown in ------------------- */
    
    struct platform_window;
    struct native_menu_item;
    struct cursor_spec {
        std::string shape = "default";
        std::vector<std::uint32_t> pixels;
        int width = 0, height = 0, hotspot_x = 0, hotspot_y = 0;
        bool hidden = false;
        bool operator==(const cursor_spec &) const = default;
    };
    
    /* Events the backend delivers; translated into NSEvent objects by app.cpp. */
    enum class event_kind { none, mouse_down, mouse_up, mouse_moved, mouse_dragged, scroll, key_down, key_up, flags_changed,
                            resized, close_requested, exposed, focus_in, focus_out, menu_action, menu_open };
    struct platform_event {
        event_kind kind = event_kind::none;
        platform_window *window = nullptr;
        point where;            /* in window coordinates, bottom-left origin */
        double dx = 0, dy = 0;  /* scroll deltas */
        int button = 0;         /* 0 left, 1 right, 2 other */
        std::uint32_t keycode = 0, modifiers = 0;
        std::string characters, characters_ignoring_modifiers;
        size new_size;
        std::uintptr_t menu_token = 0;
        std::uint32_t input_serial = 0;
    };
    
    /* The display connection. Everything is a no-op when there is none, so an
     * app can run headless (MACHOMAN_APPKIT_HEADLESS=1 or no compositor). */
    struct platform {
        static bool available();
        static platform_window *create_window(id ns_window, rect frame, const std::string &title, bool decorated, bool resizable,
                                              platform_window *popup_parent = nullptr, rect popup_anchor = {}, std::uint32_t popup_serial = 0);
        static void destroy_window(platform_window *w);
        static void show_window(platform_window *w, bool show);
        static void set_title(platform_window *w, const std::string &title);
        static std::string dialog_parent(platform_window *w);
        static void set_parent(platform_window *w, platform_window *parent);
        static void set_frame(platform_window *w, rect frame);
        static void set_size_limits(platform_window *w, size minimum, size maximum);
        static rect window_frame(platform_window *w);
        static void set_menu(platform_window *w, const std::vector<native_menu_item> &items);
        static void set_cursor(const cursor_spec &cursor);
        static clipboard_snapshot clipboard_types();
        static bool clipboard_read(const std::string &type, std::string &bytes);
        static void clipboard_write(const clipboard_data &data);
        static bool present_metal(platform_window *w, const void *layer, rect frame,
                                  const void *bgra, std::uint32_t width, std::uint32_t height, std::uint64_t stride,
                                  double contents_scale, unsigned placement);
        static void release_metal(const void *layer);
    #ifdef MM_APPKIT_EVILCOCOA
        static void present_cocoa(platform_window *w, const std::vector<evilcocoa::item> &items);
    #endif
    #ifdef MM_APPKIT_MEVK
        static bool present_metal_texture(platform_window *w, const void *layer, rect frame,
            const std::shared_ptr<mevk::Texture> &texture, double contents_scale, unsigned placement);
    #endif
        /* Waits up to `timeout_seconds` (negative: forever) for the next event;
         * false when nothing arrived. */
        static bool next_event(platform_event &out, double timeout_seconds);
        /* Wakes a blocked next_event from another thread. */
        static void wake();
        static size screen_size();
        static double screen_scale();
        static double screen_refresh();
        /* An EGL context for an NSOpenGLContext attached to `w`. */
        static void *create_gl_context(platform_window *w, void *share);
        static void destroy_gl_context(void *ctx);
        static bool make_current(void *ctx, platform_window *w);
        static void swap_buffers(void *ctx, platform_window *w);
        /* Introspection for the GL drawing probes. */
        static void *egl_display_of();
        static void *egl_surface_of(platform_window *w);
        static void set_swap_interval(void *ctx, int interval);
    };
    
    /* ---- object payloads ------------------------------------------------------ */
    
    /* Every AppKit object here starts with the objc header, so the struct can
     * be placement-new'd over the instance (see objc_framework.h). */
    struct responder_object { mm_objc_object base; id next = nullptr; };
    
    /* Printable/editing keys belong to a focused text client before window
     * and application key equivalents. Command/Control shortcuts still run. */
    bool text_input_has_priority(id window, id event);
    
    struct event_object {
        mm_objc_object base;
        std::uint64_t type = 0, modifiers = 0, click_count = 0, button = 0, keycode = 0;
        double timestamp = 0, dx = 0, dy = 0;
        bool repeat = false;
        std::uint32_t input_serial = 0;
        point where;
        id window = nullptr;
        std::string characters, characters_ignoring_modifiers;
    };
    
    struct color_object { mm_objc_object base; double r = 0, g = 0, b = 0, a = 1; };
    
    struct view_object {
        mm_objc_object base;
        id next = nullptr;          /* NSResponder's field: views are responders */
        rect frame, bounds;
        id superview = nullptr, window = nullptr, layer = nullptr;
        std::vector<id> subviews;   /* retained, back to front */
        std::int64_t tag = 0;       /* how a program finds a view it built */
        unsigned contents_placement = 0;
        unsigned autoresizing_mask = 0;
        bool autoresizes_subviews = true, translates_mask = true;
        struct layout_constraint {
            id first = nullptr, second = nullptr;
            int attribute = 0, second_attribute = 0, relation = 0;
            double multiplier = 1, constant = 0, priority = 1000;
        };
        std::vector<layout_constraint> constraints;
        struct tooltip_rect { rect frame; id owner=nullptr; void *data=nullptr; std::int64_t tag=0; };
        std::vector<tooltip_rect> tooltips;
        std::int64_t next_tooltip_tag=1;
        bool needs_display = true, hidden = false, wants_layer = false;
        bool posts_frame_notifications = true, posts_bounds_notifications = false;
    #ifdef MM_APPKIT_EVILCOCOA
        std::shared_ptr<evilcocoa::bitmap> drawing;
    src/dylibs/AppKit/appkit.h:220:void register_cocoa_controls(Class view, Class control);
    src/dylibs/AppKit/appkit.h:295:void register_bitmap_drawing();
    src/dylibs/AppKit/appkit.h:325:void register_file_panels();
    src/dylibs/AppKit/appkit.h:326:void register_window_controller();
    src/dylibs/AppKit/appkit.h:370:void register_responder();   /* NSResponder, NSEvent, NSCursor, NSColor, ... */
    src/dylibs/AppKit/appkit.h:375:void register_views();       /* NSView, NSWindow, NSViewController, NSGraphicsContext */
    src/dylibs/AppKit/appkit.h:376:void register_text_view();   /* NSTextView and field-editor text input */
    src/dylibs/AppKit/appkit.h:377:void register_stack_view();  /* NSStackView container hierarchy */
    src/dylibs/AppKit/appkit.h:378:void register_pasteboard();
    src/dylibs/AppKit/appkit.h:379:void register_application(); /* NSApplication, NSScreen, NSMenu, NSWorkspace, ... */
    src/dylibs/AppKit/appkit.h:380:void register_opengl();      /* NSOpenGLContext, NSOpenGLPixelFormat */
    src/dylibs/AppKit/appkit.h:381:void register_layers();      /* CALayer, NSOpenGLLayer, CATransaction */
    src/dylibs/Foundation/foundation.h:8: * it and -dealloc destroys it (see register_lifecycle).
    src/dylibs/Foundation/foundation.h:77:template <typename T> void register_lifecycle(Class cls) {
    src/dylibs/Foundation/foundation.h:103:template <typename T> void register_cluster(Class abstract, Class concrete, bool mutable_) {
    src/dylibs/Foundation/foundation.h:209:void dispatch_drain_main_queue();
    src/dylibs/Foundation/foundation.h:211:void *dispatch_main_queue_handle();
    src/dylibs/Foundation/foundation.h:213:void register_dispatch_data();
    src/dylibs/Foundation/foundation.h:214:void register_cookies();
    src/dylibs/Foundation/foundation.h:217:void register_strings();
    src/dylibs/Foundation/foundation.h:218:void register_formatting();
    src/dylibs/Foundation/foundation.h:221:void register_collections();
    src/dylibs/Foundation/foundation.h:222:void register_plists();
    src/dylibs/Foundation/foundation.h:223:void register_bundle();
    src/dylibs/Foundation/foundation.h:224:void register_runloop();
    src/dylibs/Foundation/foundation.h:225:void register_misc();
    src/dylibs/Foundation/foundation.h:226:void register_threading();
    src/dylibs/Foundation/foundation.h:229:void register_task();      /* NSTask, NSPipe, NSFileHandle */
    src/dylibs/Foundation/foundation.h:230:void register_key_value(); /* Manual KVO and object-valued key access. */
    542:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebView    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
    543:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebViewConfiguration    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
    6.12.0
    ✓ • 0ms

## Assistant

Стек подтвердил причину: Roblox добавляет `WKWebView` в дерево `NSView`, а нынешняя заглушка выделяет объект размером с `NSObject`. AppKit читает из него поля вида и падает.

Сделаю WebView с полноценным объектом `NSView` и браузерным движком для страницы входа. На машине доступен Qt WebEngine; свяжу его с Cocoa-конфигурацией, cookies и сообщениями JavaScript.

## Activity

    $ sed -n '115,210p' CMakeLists.txt && sed -n '205,219p' src/dylibs/Foundation/foundation.h && rg -n 'dispatch_async_f|dispatch_main_queue' src/dylibs/Foundation/dispatch.cpp && sed -n '1,200p' src/dylibs/Foundation/cookies.cpp && cat include/machoman/json.h
        target_link_libraries(libSystem PRIVATE libobjc)
    
        # CoreFoundation + Foundation: one object, since the types are the same
        # (toll-free bridging) and both frameworks' install names point at it.
        add_library(Foundation SHARED
            src/dylibs/Foundation/strings.cpp src/dylibs/Foundation/collections.cpp src/dylibs/Foundation/index_set.cpp
            src/dylibs/Foundation/plists.cpp src/dylibs/Foundation/misc.cpp src/dylibs/Foundation/task.cpp src/dylibs/Foundation/dispatch.cpp
            src/dylibs/Foundation/dispatch_data.cpp
            src/dylibs/Foundation/cookies.cpp
            src/dylibs/Foundation/key_value.cpp src/dylibs/Foundation/threading.cpp
            src/dylibs/Foundation/geometry.cpp
            src/dylibs/Foundation/formatting.cpp
            src/dylibs/symbol-synthesis/synthesise.cpp)
        target_include_directories(Foundation PRIVATE src/dylibs/symbol-synthesis)
        target_link_libraries(Foundation PRIVATE machoman_core libobjc)
        target_compile_options(Foundation PRIVATE -Wall -Wextra -fvisibility=hidden)
        set_target_properties(Foundation PROPERTIES
            BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib" INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
            PREFIX "" OUTPUT_NAME "CoreFoundation" SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A")
        add_custom_command(TARGET Foundation POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../CoreFoundation.framework/Versions/A/CoreFoundation.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.so")
    
        if(TARGET mevk)
            add_subdirectory(modules/MEVK/src/metal)
        endif()
    
        # AppKit: the object model plus a platform backend. Wayland when the
        # xdg-shell protocol definitions are available, headless otherwise.
        set(MM_APPKIT_SOURCES
            src/dylibs/AppKit/appkit_main.cpp src/dylibs/AppKit/responder.cpp
            src/dylibs/AppKit/views.cpp src/dylibs/AppKit/controls.cpp src/dylibs/AppKit/application.cpp src/dylibs/AppKit/opengl.cpp
            src/dylibs/AppKit/layers.cpp
            src/dylibs/AppKit/text_view.cpp
            src/dylibs/AppKit/stack_view.cpp
            src/dylibs/AppKit/pasteboard.cpp
            src/dylibs/AppKit/bitmap_drawing.cpp
            src/dylibs/AppKit/app_icon.cpp
            src/dylibs/AppKit/icon_image.cpp
            src/dylibs/AppKit/native_menu.cpp
            src/dylibs/AppKit/native_dialog.cpp src/dylibs/AppKit/file_panels.cpp
            src/dylibs/AppKit/window_controller.cpp
            src/dylibs/AppKit/nib.cpp src/dylibs/AppKit/nib_archive.cpp)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MM_WAYLAND QUIET wayland-client wayland-cursor wayland-egl egl xkbcommon)
            pkg_check_modules(MM_PANGO QUIET IMPORTED_TARGET pangocairo)
            pkg_check_modules(MM_PIXBUF QUIET IMPORTED_TARGET gdk-pixbuf-2.0)
            pkg_check_modules(MM_GIO QUIET IMPORTED_TARGET gio-2.0)
            pkg_check_modules(MM_GTK QUIET IMPORTED_TARGET gtk+-3.0)
            pkg_get_variable(MM_WAYLAND_PROTOCOLS wayland-protocols pkgdatadir)
            find_program(MM_WAYLAND_SCANNER wayland-scanner)
        endif()
        if(MM_WAYLAND_FOUND AND MM_WAYLAND_PROTOCOLS AND MM_WAYLAND_SCANNER)
            # xdg-shell has no library: its client code is generated from the
            # protocol definition shipped by wayland-protocols.
            set(MM_XDG_XML "${MM_WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml")
            set(MM_XDG_DIR "${CMAKE_BINARY_DIR}/generated/wayland")
            file(MAKE_DIRECTORY "${MM_XDG_DIR}")
            add_custom_command(OUTPUT "${MM_XDG_DIR}/xdg-shell-client-protocol.h"
                COMMAND ${MM_WAYLAND_SCANNER} client-header "${MM_XDG_XML}" "${MM_XDG_DIR}/xdg-shell-client-protocol.h"
                DEPENDS "${MM_XDG_XML}" VERBATIM)
            add_custom_command(OUTPUT "${MM_XDG_DIR}/xdg-shell-protocol.c"
                COMMAND ${MM_WAYLAND_SCANNER} private-code "${MM_XDG_XML}" "${MM_XDG_DIR}/xdg-shell-protocol.c"
                DEPENDS "${MM_XDG_XML}" VERBATIM)
            list(APPEND MM_APPKIT_SOURCES src/dylibs/AppKit/platform_wayland.cpp src/dylibs/AppKit/clipboard_wayland.cpp
                 "${MM_XDG_DIR}/xdg-shell-protocol.c" "${MM_XDG_DIR}/xdg-shell-client-protocol.h")
            set(MM_DATA_CONTROL_XML "${MM_WAYLAND_PROTOCOLS}/staging/ext-data-control/ext-data-control-v1.xml")
            if(EXISTS "${MM_DATA_CONTROL_XML}")
                add_custom_command(OUTPUT "${MM_XDG_DIR}/ext-data-control-v1-client-protocol.h"
                    COMMAND ${MM_WAYLAND_SCANNER} client-header "${MM_DATA_CONTROL_XML}" "${MM_XDG_DIR}/ext-data-control-v1-client-protocol.h"
                    DEPENDS "${MM_DATA_CONTROL_XML}" VERBATIM)
                add_custom_command(OUTPUT "${MM_XDG_DIR}/ext-data-control-v1-protocol.c"
                    COMMAND ${MM_WAYLAND_SCANNER} private-code "${MM_DATA_CONTROL_XML}" "${MM_XDG_DIR}/ext-data-control-v1-protocol.c"
                    DEPENDS "${MM_DATA_CONTROL_XML}" VERBATIM)
                list(APPEND MM_APPKIT_SOURCES "${MM_XDG_DIR}/ext-data-control-v1-client-protocol.h" "${MM_XDG_DIR}/ext-data-control-v1-protocol.c")
            endif()
            set(MM_FOREIGN_XML "${MM_WAYLAND_PROTOCOLS}/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml")
            if(EXISTS "${MM_FOREIGN_XML}")
                add_custom_command(OUTPUT "${MM_XDG_DIR}/xdg-foreign-v2-client-protocol.h"
                    COMMAND ${MM_WAYLAND_SCANNER} client-header "${MM_FOREIGN_XML}" "${MM_XDG_DIR}/xdg-foreign-v2-client-protocol.h"
                    DEPENDS "${MM_FOREIGN_XML}" VERBATIM)
                add_custom_command(OUTPUT "${MM_XDG_DIR}/xdg-foreign-v2-protocol.c"
                    COMMAND ${MM_WAYLAND_SCANNER} private-code "${MM_FOREIGN_XML}" "${MM_XDG_DIR}/xdg-foreign-v2-protocol.c"
                    DEPENDS "${MM_FOREIGN_XML}" VERBATIM)
                list(APPEND MM_APPKIT_SOURCES "${MM_XDG_DIR}/xdg-foreign-v2-client-protocol.h" "${MM_XDG_DIR}/xdg-foreign-v2-protocol.c")
            endif()
            set(MM_ICON_XML "${MM_WAYLAND_PROTOCOLS}/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml")
            if(EXISTS "${MM_ICON_XML}")
                add_custom_command(OUTPUT "${MM_XDG_DIR}/xdg-toplevel-icon-v1-client-protocol.h"
                    COMMAND ${MM_WAYLAND_SCANNER} client-header "${MM_ICON_XML}" "${MM_XDG_DIR}/xdg-toplevel-icon-v1-client-protocol.h"
                    DEPENDS "${MM_ICON_XML}" VERBATIM)
                add_custom_command(OUTPUT "${MM_XDG_DIR}/xdg-toplevel-icon-v1-protocol.c"
    bool is_url(id o);
    
    /* Runs whatever is waiting on the main dispatch queue (dispatch.cpp).
     * The run loop calls this once per turn: the main queue is the run loop. */
    void dispatch_drain_main_queue();
    void dispatch_submit_owned_block(void *queue, id block);
    void *dispatch_main_queue_handle();
    void dispatch_submit_callback(void *queue, void *function, void *context);
    void register_dispatch_data();
    void register_cookies();
    
    /* Registration entry points (one per translation unit). */
    void register_strings();
    void register_formatting();
    std::string format_call_arguments(id fmt, unsigned named_integer_args);
    156:void *dispatch_main_queue_handle() { return &main_queue(); }
    207:void darwin_dispatch_async_f(dispatch_queue *queue, void *context, void *function) DARWIN(dispatch_async_f);
    208:void darwin_dispatch_async_f(dispatch_queue *queue, void *context, void *function) {
    /* Cocoa's cookie bridge is also used by applications with their own HTTP
     * engine (Roblox uses guest libcurl). No network requests happen here. */
    #include "foundation.h"
    
    #include <algorithm>
    #include <charconv>
    #include <cctype>
    #include <chrono>
    #include <cmath>
    #include <cstdio>
    #include <cstdlib>
    #include <ctime>
    #include <mutex>
    #include <unordered_map>
    
    FND_CLASS(NSHTTPCookie);
    FND_CLASS(NSHTTPCookieStorage);
    
    #define COOKIE_KEY(name) extern "C" { EXPORT fnd::id name##_ __asm__("_" #name) = nullptr; }
    COOKIE_KEY(NSHTTPCookieName)
    COOKIE_KEY(NSHTTPCookieValue)
    COOKIE_KEY(NSHTTPCookieDomain)
    COOKIE_KEY(NSHTTPCookiePath)
    COOKIE_KEY(NSHTTPCookieVersion)
    COOKIE_KEY(NSHTTPCookieExpires)
    COOKIE_KEY(NSHTTPCookieSecure)
    COOKIE_KEY(NSHTTPCookieDiscard)
    COOKIE_KEY(NSHTTPCookieMaximumAge)
    COOKIE_KEY(NSHTTPCookieOriginURL)
    COOKIE_KEY(NSHTTPCookiePort)
    COOKIE_KEY(NSHTTPCookieComment)
    COOKIE_KEY(NSHTTPCookieCommentURL)
    COOKIE_KEY(NSHTTPCookieManagerCookiesChangedNotification)
    COOKIE_KEY(NSHTTPCookieManagerAcceptPolicyChangedNotification)
    #undef COOKIE_KEY
    
    namespace fnd {
    namespace {
    Class cookie_class, storage_class;
    struct cookie_object {
        mm_objc_object base;
        id properties = nullptr;
        std::string name, value, domain, path;
        double expires = 0;
        std::uint64_t version = 0;
        bool secure = false, http_only = false, session = true, host_only = true, has_expiry = false;
        ~cookie_object() { release(properties); }
    };
    struct storage_object {
        mm_objc_object base;
        std::mutex mutex;
        std::vector<id> cookies;
        std::uint64_t policy = 0; // NSHTTPCookieAcceptPolicyAlways
        ~storage_object() { for (id cookie : cookies) release(cookie); }
    };
    std::string lower(std::string_view s) {
        std::string out(s);
        for (char &c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        return out;
    }
    std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
        return s;
    }
    double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    id property(id d, const char *key) { return dictionary_get(d, constant_string(key)); }
    void put(id d, const char *key, std::string_view value) {
        id text = string_from_utf8(value); dictionary_set(d, constant_string(key), text); release(text);
    }
    bool truth(id value) {
        if (is_number(value)) return number_int64(value) != 0;
        auto text = lower(to_utf8(value));
        return text == "true" || text == "yes" || text == "1";
    }
    bool integer(std::string_view text, std::int64_t &result) {
        text = trim(text);
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    bool domain_matches(std::string_view host, std::string_view domain, bool host_only) {
        while (!domain.empty() && domain.front() == '.') domain.remove_prefix(1);
        return host == domain || (!host_only && host.size() > domain.size() && host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.');
    }
    struct cookie_url { std::string host, path; bool secure = false; };
    cookie_url parse_url(id url) {
        cookie_url result;
        if (!is_url(url)) return result;
        std::string text = to_utf8(obj(send(url, "absoluteString")));
        auto sep = text.find("://");
        if (sep == std::string::npos) return result;
        auto scheme = lower(std::string_view(text).substr(0, sep));
        if (scheme != "http" && scheme != "https") return result;
        result.secure = scheme == "https";
        std::string_view rest(text); rest.remove_prefix(sep + 3);
        auto end = rest.find_first_of("/?#");
        auto authority = rest.substr(0, end);
        if (auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
        if (authority.starts_with('[')) {
            auto close = authority.find(']');
            if (close == std::string_view::npos) return {};
            result.host = lower(authority.substr(0, close + 1));
        } else result.host = lower(authority.substr(0, authority.find(':')));
        result.path = end != std::string_view::npos && rest[end] == '/' ? std::string(rest.substr(end, rest.find_first_of("?#", end) - end)) : "/";
        return result;
    }
    bool cookie_date(std::string_view text, double &result) {
        // RFC 6265 date tokens also cover RFC 850 and asctime cookie dates.
        int day = -1, month = -1, year = -1, hour = -1, minute = -1, second = -1;
        constexpr std::string_view months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
        std::string s = lower(text);
        std::size_t pos = 0;
        while (pos < s.size()) {
            while (pos < s.size() && !(std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == ':')) ++pos;
            auto start = pos;
            while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == ':')) ++pos;
            if (start == pos) break;
            auto token = std::string_view(s).substr(start, pos - start);
            int h, m, sec;
            std::string token_string(token);
            if (hour < 0 && std::sscanf(token_string.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) { hour = h; minute = m; second = sec; continue; }
            if (month < 0 && token.size() >= 3) {
                for (int i = 0; i < 12; ++i) if (token.substr(0, 3) == months[i]) { month = i; break; }
                if (month >= 0) continue;
            }
            std::int64_t number;
            if (!integer(token, number)) continue;
            if (day < 0 && token.size() <= 2 && number >= 1 && number <= 31) day = static_cast<int>(number);
            else if (year < 0 && token.size() >= 2 && token.size() <= 4) year = static_cast<int>(number);
        }
        if (year >= 0 && year <= 69) year += 2000;
        else if (year >= 70 && year <= 99) year += 1900;
        if (year < 1601 || month < 0 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
        std::tm tm{}; tm.tm_year = year - 1900; tm.tm_mon = month; tm.tm_mday = day; tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second;
        result = static_cast<double>(timegm(&tm)); return true;
    }
    bool initialize_cookie(cookie_object *cookie, id properties) {
        if (!is_dictionary(properties)) return false;
        id name = property(properties, "Name"), value = property(properties, "Value");
        if (!is_string(name) || !is_string(value)) return false;
        cookie->name = to_utf8(name); cookie->value = to_utf8(value);
        cookie->domain = lower(to_utf8(property(properties, "Domain")));
        if (cookie->domain.empty()) cookie->domain = parse_url(property(properties, "OriginURL")).host;
        cookie->path = to_utf8(property(properties, "Path"));
        if (cookie->name.empty() || cookie->name.find_first_of("()<>@,;:\\\"/[]?={} \t\r\n") != std::string::npos || cookie->value.find_first_of("\r\n;") != std::string::npos || cookie->domain.empty() || cookie->path.empty() || cookie->path[0] != '/') return false;
        cookie->host_only = cookie->domain.front() != '.';
        cookie->secure = truth(property(properties, "Secure")); cookie->http_only = truth(property(properties, "HTTPOnly"));
        id version = property(properties, "Version");
        std::int64_t number = 0;
        if (is_number(version)) number = number_int64(version); else integer(to_utf8(version), number);
        cookie->version = number > 0 ? static_cast<std::uint64_t>(number) : 0;
        id expiry = property(properties, "Expires");
        bool has_expiry = expiry && is_kind(expiry, cls("NSDate"));
        if (has_expiry) {
            cookie->expires = date_to_epoch(expiry);
        } else if (is_string(expiry)) has_expiry = cookie_date(to_utf8(expiry), cookie->expires);
        id max_age = property(properties, "Max-Age");
        if (max_age && (is_number(max_age) || integer(to_utf8(max_age), number))) {
            if (is_number(max_age)) number = number_int64(max_age);
            cookie->expires = number <= 0 ? 1 : now() + static_cast<double>(number); has_expiry = true;
        }
        cookie->session = !has_expiry || truth(property(properties, "Discard"));
        cookie->has_expiry = has_expiry;
        release(cookie->properties);
        cookie->properties = obj(send(properties, "copy"));
        return true;
    }
    id new_cookie(id properties) {
        auto *cookie = make<cookie_object>(cookie_class);
        if (!initialize_cookie(cookie, properties)) { release(reinterpret_cast<id>(cookie)); return nullptr; }
        return reinterpret_cast<id>(cookie);
    }
    std::uint64_t cookie_init(id self, SEL, id properties) {
        if (!initialize_cookie(as<cookie_object>(self), properties)) { release(self); return 0; }
        return ret(self);
    }
    std::uint64_t cookie_create(id, SEL, id properties) { return ret(autorelease(new_cookie(properties))); }
    
    // Split folded Set-Cookie fields without splitting the comma inside Expires.
    std::vector<std::string_view> split_cookies(std::string_view header) {
        std::vector<std::string_view> result;
        std::size_t start = 0; bool quoted = false;
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == '"') quoted = !quoted;
            if (quoted || (header[i] != ',' && header[i] != '\n')) continue;
            auto rest = trim(header.substr(i + 1));
            auto equal = rest.find('='); auto delimiter = rest.find_first_of(";,\r\n");
            if (header[i] == '\n' || (equal != std::string_view::npos && (delimiter == std::string_view::npos || equal < delimiter))) {
                result.push_back(trim(header.substr(start, i - start))); start = i + 1;
            }
        }
        result.push_back(trim(header.substr(start))); return result;
    }
    std::uint64_t cookies_response(id, SEL, id headers, id url) {
        std::vector<id> cookies;
        auto origin = parse_url(url);
        if (is_dictionary(headers) && !origin.host.empty()) {
            std::vector<id> keys; dictionary_keys(headers, keys);
            for (id key : keys) {
                if (lower(to_utf8(key)) != "set-cookie") continue;
    #ifndef MACHOMAN_JSON_H
    #define MACHOMAN_JSON_H
    #include <map>
    #include <string>
    #include <string_view>
    #include <variant>
    #include <vector>
    
    /* Lossless JSON numbers preserve unknown manifest fields without conversion
     * through floating point. Objects reject duplicate keys when parsed. */
    struct mm_json_number { std::string text; };
    struct mm_json {
        using object = std::map<std::string, mm_json, std::less<>>;
        using array = std::vector<mm_json>;
        std::variant<std::nullptr_t, bool, mm_json_number, std::string, object, array> value = nullptr;
        mm_json() = default;
        template<class T> explicit mm_json(T data) : value(std::move(data)) {}
    };
    bool mm_json_parse(std::string_view source, mm_json &out, std::string &error);
    std::string mm_json_serialize(const mm_json &value);
    #endif
    ✓ • 257ms

## Activity

    $ sed -n '145,183p' src/dylibs/AppKit/appkit.h && sed -n '1,105p' src/dylibs/AppKit/appkit_main.cpp && rg -n 'JSONSerialization|JSONObjectWithData|dataWithJSONObject|NSError|errorWithDomain' src/dylibs/Foundation/*.cpp | head -n 30 && rg -n 'mm_host_api|dylib_init|dylib_stop|init' modules/MEVK/src/metal/metal.cpp | tail -n 20
        id next = nullptr;          /* NSResponder's field: views are responders */
        rect frame, bounds;
        id superview = nullptr, window = nullptr, layer = nullptr;
        std::vector<id> subviews;   /* retained, back to front */
        std::int64_t tag = 0;       /* how a program finds a view it built */
        unsigned contents_placement = 0;
        unsigned autoresizing_mask = 0;
        bool autoresizes_subviews = true, translates_mask = true;
        struct layout_constraint {
            id first = nullptr, second = nullptr;
            int attribute = 0, second_attribute = 0, relation = 0;
            double multiplier = 1, constant = 0, priority = 1000;
        };
        std::vector<layout_constraint> constraints;
        struct tooltip_rect { rect frame; id owner=nullptr; void *data=nullptr; std::int64_t tag=0; };
        std::vector<tooltip_rect> tooltips;
        std::int64_t next_tooltip_tag=1;
        bool needs_display = true, hidden = false, wants_layer = false;
        bool posts_frame_notifications = true, posts_bounds_notifications = false;
    #ifdef MM_APPKIT_EVILCOCOA
        std::shared_ptr<evilcocoa::bitmap> drawing;
    #endif
        ~view_object();
    };
    
    struct control_object {
        view_object view;
        id title = nullptr, key_equivalent = nullptr, target = nullptr, image = nullptr, font = nullptr;
        id attributed_title = nullptr;
        id placeholder = nullptr, identifier = nullptr, text_field = nullptr, image_view = nullptr;
        SEL action = nullptr, double_action = nullptr;
        std::int64_t state = 0, button_type = 0, bezel_style = 1, alignment = 0, image_scaling = 0;
        std::string font_family="Sans";
        double value = 0, minimum = 0, maximum = 1, knob = .25, font_size = 13;
        bool enabled = true, highlighted = false, bordered = true, bezeled = true;
        bool editable = false, selectable = false, draws_background = false, bold = false, wraps = false;
        bool indeterminate = false, animating = false, vertical = false;
        bool menu_row = false;
        id box_content = nullptr; /* weak; retained by view.subviews */
    /* AppKit's entry point: registers every class the framework defines and
     * keeps the runtime's host API. The classes are registered from a
     * constructor, because the guest's bind records are resolved before any
     * guest code runs and they must already exist. */
    #include "appkit.h"
    #include "synthesise.h"
    
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <string>
    
    namespace ak {
    
    namespace {
    const mm_host_api *g_api = nullptr;
    bool g_trace = false;
    } // namespace
    
    void set_api(const mm_host_api *a) { g_api = a; }
    
    const mm_host_api *api() { return g_api; }
    bool trace() {
        /* MACHOMAN_APPKIT_TRACE traces the window system alone; MACHOMAN_TRACE
         * also traces every host call, which is far too much to read. */
        static const bool on = std::getenv("MACHOMAN_TRACE") != nullptr ||
                               std::getenv("MACHOMAN_APPKIT_TRACE") != nullptr;
        g_trace = on;
        return on;
    }
    
    std::string to_utf8(id nsstring) {
        if (!nsstring) return {};
        if (is_kind(nsstring, cls("NSAttributedString"))) nsstring=obj(send(nsstring,"string"));
        const char *utf8 = reinterpret_cast<const char *>(send(nsstring, "UTF8String"));
        return utf8 ? std::string(utf8) : std::string();
    }
    id string(const std::string &utf8) {
        return obj(send(reinterpret_cast<id>(cls("NSString")), "stringWithUTF8String:", ret(utf8.c_str())));
    }
    id number(double v) {
        mm_cpu_context *ctx = g_api ? g_api->current_context() : nullptr;
        if (ctx) std::memcpy(&ctx->v[0][0], &v, 8);
        return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithDouble:"));
    }
    
    } // namespace ak
    
    extern "C" {
    
    __attribute__((visibility("default"))) void machoman_dylib_init(const mm_host_api *api) { ak::set_api(api); }
    
    /* libobjc and Foundation are dependencies, so their constructors (and
     * therefore NSObject and NSString) have already run when this one does. */
    static void appkit_ctor() __attribute__((constructor));
    static void appkit_ctor() {
        ak::set_api(mm_objc_host_api());
        ak::register_responder();
        ak::register_pasteboard();
        ak::register_views();
        ak::register_text_view();
        ak::register_stack_view();
        ak::register_application();
        ak::register_file_panels();
        ak::register_window_controller();
        ak::register_opengl();
        ak::register_layers();
    }
    
    } // extern "C"
    
    /* AppKit defines its classes but not yet every constant the guest binds
     * to; those are synthesised from their names (see symbol-synthesis). */
    extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
        return mm_synthesise_symbol(name, flags);
    }
    
    /* The public application entry point belongs to AppKit and is re-exported
     * by the Cocoa and Quartz umbrellas. Bundle metadata selects the application
     * class and main nib; -run delivers the launch notifications and event loop. */
    extern "C" int NSApplicationMain_(int, const char **) DARWIN(NSApplicationMain);
    extern "C" int NSApplicationMain_(int, const char **) {
        using namespace ak;
        autorelease_pool pool;
        id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
        auto info = [&](const char *key) { return obj(send(bundle, "objectForInfoDictionaryKey:", ret(string(key)))); };
        auto principal = to_utf8(info("NSPrincipalClass"));
        Class application = principal.empty() ? cls("NSApplication") : cls(principal.c_str());
        if (!application) {
            std::fprintf(stderr, "AppKit: NSApplicationMain: unknown principal class %s\n", principal.c_str());
            return 1;
        }
        id app = obj(send(reinterpret_cast<id>(application), "sharedApplication"));
        const auto nib = to_utf8(info("NSMainNibFile"));
        std::vector<id> top_level;
        if (!nib.empty() && !load_nib(nib, app, &top_level)) {
            std::fprintf(stderr, "AppKit: NSApplicationMain: unable to load %s\n", nib.c_str());
            return 1;
        }
        // Top-level objects must outlive the autorelease pool of nib decoding.
        for (id object : top_level) retain(object);
        send(app, "run");
        for (id object : top_level) release(object);
        return 0;
    }
    src/dylibs/Foundation/task.cpp:294:            *error = obj(send(reinterpret_cast<id>(cls("NSError")), "errorWithDomain:code:userInfo:",
    src/dylibs/Foundation/misc.cpp:2: * allocators, NSURL/CFURL, NSError/CFError, NSBundle/CFBundle,
    src/dylibs/Foundation/misc.cpp:47:FND_CLASS(NSURL); FND_CLASS(NSError); FND_CLASS(NSBundle); FND_CLASS(NSUserDefaults); FND_CLASS(NSProcessInfo);
    src/dylibs/Foundation/misc.cpp:578:/* ---- NSError --------------------------------------------------------------- */
    src/dylibs/Foundation/misc.cpp:603:std::uint64_t errc_errorWithDomainCodeUserInfo(id c, SEL, id domain, std::uint64_t code, id info) {
    src/dylibs/Foundation/misc.cpp:1748:    g_error = mm_objc_define_class("NSError", nsobject, sizeof(error_object), FND_CLASS_REF(NSError), FND_META_REF(NSError));
    src/dylibs/Foundation/misc.cpp:1758:    FND_CLASS_METHODS(g_error, {"errorWithDomain:code:userInfo:", IMP(errc_errorWithDomainCodeUserInfo), "@40@0:8@16q24@32"});
    src/dylibs/Foundation/misc.cpp:2098:CFTypeID CFErrorGetTypeID_() { return ret(cls("NSError")); }
    350:extern "C" MM_FW_EXPORT void machoman_dylib_stop() {
    596:MM_FW_EXPORT void machoman_dylib_init(const mm_host_api *) {
    ✓ • 263ms

## Activity

    $ rg -n 'block_descriptor|block_literal|NSConcrete.*Block|completionHandler|mm_objc_block_invoke' src/dylibs/AppKit src/dylibs/Foundation include/machoman/objc_host.h | head -n 55 && sed -n '235,310p' CMakeLists.txt && sed -n '1,110p' include/machoman/guest_args.h && cat modules/evilcocoa/CMakeLists.txt | head -n 80
    include/machoman/objc_host.h:109:MM_OBJC_API uint64_t mm_objc_block_invoke(void *block, int nargs, const uint64_t *args);
    src/dylibs/Foundation/threading.cpp:232:                mm_objc_block_invoke(t->block, 0, nullptr);
    src/dylibs/Foundation/task.cpp:304:        mm_objc_block_invoke(handler, 1, &arg);
    src/dylibs/AppKit/file_panels.cpp:102:    if (handler) { mm_objc_block_invoke(handler, 1, &response); release(handler); }
    src/dylibs/AppKit/file_panels.cpp:177:        {"beginSheetModalForWindow:completionHandler:", MM_IMP(begin_sheet), "v32@0:8@16@?24"},
    src/dylibs/Foundation/misc.cpp:1237:    if (block) mm_objc_block_invoke(block, 1, &arg);
    src/dylibs/Foundation/misc.cpp:1378:            mm_objc_block_invoke(block.get(), 0, nullptr);
    src/dylibs/Foundation/misc.cpp:1577:                mm_objc_block_invoke(item->block, 1, &argument);
    src/dylibs/Foundation/misc.cpp:1582:        } else if (e.block) mm_objc_block_invoke(e.block, 1, reinterpret_cast<const std::uint64_t *>(&note));
    src/dylibs/AppKit/application.cpp:871:            if (handler) { mm_objc_block_invoke(handler, 1, &response); release(handler); }
    src/dylibs/AppKit/application.cpp:1154:        {"beginSheetModalForWindow:completionHandler:", MM_IMP(alert_beginSheetModal), "v32@0:8@16@?24"});
    src/dylibs/Foundation/dispatch_data.cpp:143:        const bool proceed = (mm_objc_block_invoke(applier, 4, args) & 0xff) != 0;
    src/dylibs/Foundation/dispatch.cpp:58:        mm_objc_block_invoke(item.block, 0, nullptr);
    src/dylibs/Foundation/dispatch.cpp:219:    if (block) mm_objc_block_invoke(block, 0, nullptr);
    src/dylibs/Foundation/dispatch.cpp:258:        if (block) mm_objc_block_invoke(block, 1, &index);
    src/dylibs/Foundation/collections.cpp:181:        mm_objc_block_invoke(block, 3, args);
    src/dylibs/Foundation/collections.cpp:187:    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { const std::uint64_t args[2] = {ret(a), ret(b)}; return static_cast<std::int64_t>(mm_objc_block_invoke(block, 2, args)) < 0; });
    src/dylibs/Foundation/collections.cpp:295:    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { const std::uint64_t args[2] = {ret(a), ret(b)}; return static_cast<std::int64_t>(mm_objc_block_invoke(block, 2, args)) < 0; });
    src/dylibs/Foundation/collections.cpp:356:        mm_objc_block_invoke(block, 3, args);
        endif()
        if(TARGET evilcocoa)
            target_sources(AppKit PRIVATE src/dylibs/AppKit/evilcocoa_adapter.cpp)
            target_link_libraries(AppKit PRIVATE evilcocoa::evilcocoa)
            target_compile_definitions(AppKit PRIVATE MM_APPKIT_EVILCOCOA=1)
        endif()
        find_package(PNG QUIET)
        if(PNG_FOUND)
            target_link_libraries(AppKit PRIVATE PNG::PNG)
            target_compile_definitions(AppKit PRIVATE MM_APPKIT_PNG=1)
        endif()
        if(EXISTS "${MM_FOREIGN_XML}")
            target_compile_definitions(AppKit PRIVATE MM_APPKIT_XDG_FOREIGN=1)
        endif()
        if(EXISTS "${MM_ICON_XML}")
            target_compile_definitions(AppKit PRIVATE MM_APPKIT_XDG_ICON=1)
        endif()
        target_include_directories(AppKit PRIVATE src/dylibs/symbol-synthesis)
        target_link_libraries(AppKit PRIVATE machoman_core libobjc Foundation)
        if(MM_PANGO_FOUND)
            target_link_libraries(AppKit PRIVATE PkgConfig::MM_PANGO)
            target_compile_definitions(AppKit PRIVATE MM_APPKIT_PANGO=1)
        endif()
        if(TARGET MEVK_Metal)
            target_link_libraries(AppKit PRIVATE MEVK_Metal MEVK::mevk)
            target_compile_definitions(AppKit PRIVATE MM_APPKIT_MEVK=1)
        endif()
        target_compile_options(AppKit PRIVATE -Wall -Wextra -fvisibility=hidden)
        if(MM_WAYLAND_FOUND AND MM_WAYLAND_PROTOCOLS AND MM_WAYLAND_SCANNER)
            target_include_directories(AppKit PRIVATE "${MM_XDG_DIR}" ${MM_WAYLAND_INCLUDE_DIRS})
            target_link_libraries(AppKit PRIVATE ${MM_WAYLAND_LIBRARIES})
            target_link_directories(AppKit PRIVATE ${MM_WAYLAND_LIBRARY_DIRS})
        endif()
        set_target_properties(AppKit PROPERTIES
            BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A"
            INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A"
            PREFIX "" OUTPUT_NAME "AppKit" SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/AppKit.framework/Versions/C")
        if(TARGET MEVK_Metal)
            # Cocoa/QuartzCore can be loaded before the guest imports Metal. Their
            # AppKit alias must be able to resolve its native Metal.so dependency.
            set_property(TARGET AppKit APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:MEVK_Metal>")
            set_property(TARGET AppKit APPEND PROPERTY INSTALL_RPATH "$<TARGET_FILE_DIR:MEVK_Metal>")
        endif()
    
        # Quartz re-exports AppKit, including NSApplicationMain.
        add_custom_command(TARGET AppKit POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Quartz.framework/Versions/A"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../AppKit.framework/Versions/C/AppKit.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Quartz.framework/Versions/A/Quartz.so")
    
        # Cocoa is an umbrella over AppKit and Foundation; pointing it at AppKit
        # (which depends on Foundation) is what re-exporting means here.
        add_custom_command(TARGET AppKit POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Cocoa.framework/Versions/A"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../AppKit.framework/Versions/C/AppKit.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa.so"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/QuartzCore.framework/Versions/A"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../AppKit.framework/Versions/C/AppKit.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore.so"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/OpenGL.framework/Versions/A"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../AppKit.framework/Versions/C/AppKit.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL.so")
    
    
        # Shared PCM engine for the framework facades and platform transports.
        # The null driver is offline only; physical device enumeration waits for
        # the PipeWire start/render/stop path.
        add_library(audio SHARED src/dylibs/audio/coremidi.cpp src/dylibs/audio/coreaudio_hal.cpp
            src/dylibs/audio/audiounit.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
        target_link_libraries(audio PRIVATE machoman_core libobjc)
        target_include_directories(audio PRIVATE src/dylibs/symbol-synthesis)
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
    cmake_minimum_required(VERSION 3.25)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        project(evilcocoa LANGUAGES C CXX)
        set(CMAKE_CXX_STANDARD 20)
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EVILCOCOA_GL REQUIRED IMPORTED_TARGET epoxy)
    pkg_check_modules(EVILCOCOA_TEXT REQUIRED IMPORTED_TARGET pangocairo)
    add_library(evilcocoa_lzfse STATIC third_party/lzfse/lzfse_decode.c third_party/lzfse/lzfse_decode_base.c
        third_party/lzfse/lzfse_fse.c third_party/lzfse/lzvn_decode_base.c)
    set_target_properties(evilcocoa_lzfse PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    add_library(evilcocoa STATIC src/renderer.cpp src/asset_catalog.cpp)
    target_include_directories(evilcocoa PRIVATE third_party/lzfse)
    target_link_libraries(evilcocoa PRIVATE evilcocoa_lzfse)
    add_library(evilcocoa::evilcocoa ALIAS evilcocoa)
    target_include_directories(evilcocoa PUBLIC include)
    target_link_libraries(evilcocoa PRIVATE PkgConfig::EVILCOCOA_GL PkgConfig::EVILCOCOA_TEXT)
    target_compile_features(evilcocoa PUBLIC cxx_std_20)
    set_target_properties(evilcocoa PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(evilcocoa PRIVATE -Wall -Wextra)
    
    option(EVILCOCOA_BUILD_TESTS "Build OpenGL framebuffer checks" ON)
    if(EVILCOCOA_BUILD_TESTS)
        enable_testing()
        pkg_check_modules(EVILCOCOA_EGL REQUIRED IMPORTED_TARGET egl)
        add_executable(test_evilcocoa tests/test_renderer.cpp)
        target_link_libraries(test_evilcocoa PRIVATE evilcocoa PkgConfig::EVILCOCOA_EGL PkgConfig::EVILCOCOA_GL)
        add_test(NAME evilcocoa_renderer COMMAND test_evilcocoa)
        set_tests_properties(evilcocoa_renderer PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1" TIMEOUT 15)
    endif()
    if(EVILCOCOA_BUILD_TESTS)
        add_executable(test_evilcocoa_assets tests/test_asset_catalog.cpp)
        target_link_libraries(test_evilcocoa_assets PRIVATE evilcocoa)
        add_test(NAME evilcocoa_assets COMMAND test_evilcocoa_assets)
    endif()
    ✓ • 0ms

## Activity

    $ sed -n '960,1055p' src/dylibs/libobjc/objc.cpp && sed -n '1560,1590p' src/dylibs/libobjc/objc.cpp && rg -n 'NSConcrete.*Block|struct block_' src/dylibs/libobjc/objc.cpp | head -n 20 && sed -n '198,215p' src/dylibs/Foundation/dispatch.cpp && sed -n '580,600p' src/dylibs/Foundation/misc.cpp
                    const std::uint64_t a = reinterpret_cast<std::uintptr_t>(sel);
                    const auto target = reinterpret_cast<id>(send_host_or_guest(receiver, *f, 1, &a));
                    if (target && target != receiver) {
                        ctx->x[stret ? 1 : 0] = reinterpret_cast<std::uintptr_t>(target);
                        return dispatch(ctx, target, sel, target->isa, stret);
                    }
                }
                m = resolve_stub(c, sel);
            }
            if (!m) {
                /* A class machoman implements is always partial: Apple's have
                 * hundreds of methods and this has the ones programs were seen
                 * to use. Stopping the process at the first one that is missing
                 * makes every further gap invisible, so a host class reports it
                 * and answers nil — the same shape an unimplemented framework
                 * symbol has. A guest class is different: there the method
                 * really should exist, so that stays fatal.
                 * $MACHOMAN_STRICT_SELECTORS=1 makes both fatal. */
                static const bool strict = std::getenv("MACHOMAN_STRICT_SELECTORS") != nullptr;
                /* "A host class" means one machoman implements anywhere up the
                 * chain: an app's own NSWindow subclass inherits the gaps of
                 * the NSWindow here, and the missing method is this project's
                 * to add, not the app's. */
                bool host_class = false;
                for (Class c = start; c && !host_class; c = c->superclass)
                    host_class = rw(c) && rw(c)->host;
                std::fprintf(stderr, "objc: %c[%s %s]: unrecognized selector sent to %p (from 0x%llx)%s\n",
                             r->is_meta ? '+' : '-', r->name.c_str(), sel, static_cast<void *>(receiver),
                             static_cast<unsigned long long>(ctx->x[30]),
                             host_class && !strict ? "; answering nil" : "");
                if (!host_class || strict) std::abort();
                ctx->x[0] = 0;
                ctx->x[1] = 0;
                std::memset(ctx->v, 0, sizeof(ctx->v[0]) * 4);
                ctx->host_tail_fn = 0;
                ctx->resume_va = 0;
                ctx->host_result_in_context = 1;
                return 0;
            }
            implementation = *m;
        }
        const auto *m = &implementation;
        if (m->stub) {
            ctx->host_tail_fn = 0;
            ctx->resume_va = 0;
            auto result = invoke_stub(*ctx, *m->stub);
            ctx->host_result_in_context = 1;
            return result;
        }
        if (trace()) std::fprintf(stderr, "objc: %c[%s %s] -> %s %p\n", rw(start)->is_meta ? '+' : '-', rw(start)->name.c_str(), sel,
                                  m->guest ? "guest" : "host", m->imp);
        if (backtrace_selector(sel)) {
            std::fprintf(stderr, "objc: %c[%s %s] sent from:\n", rw(start)->is_meta ? '+' : '-', rw(start)->name.c_str(), sel);
            if (g_api && g_api->print_guest_backtrace) g_api->print_guest_backtrace();
        }
        if (m->guest) ctx->resume_va = reinterpret_cast<std::uintptr_t>(m->imp);
        else if (ctx->native_mode && (stret || (m->types && std::strchr(m->types, '{')))) {
            const auto sig = stub_abi::signature(m->types ? m->types : "");
            if (sig.empty()) {
                std::fprintf(stderr, "objc: cannot bridge Intel aggregate method %s with encoding %s\n", sel, m->types ? m->types : "(missing)");
                std::abort();
            }
            return native_abi::call_host(*ctx, m->imp, sig, reinterpret_cast<std::uintptr_t>(receiver),
                                        reinterpret_cast<std::uintptr_t>(sel), stret ? ctx->x[0] : 0);
        } else ctx->host_tail_fn = reinterpret_cast<std::uintptr_t>(m->imp);
        return stret ? ctx->x[0] : reinterpret_cast<std::uintptr_t>(receiver);
    }
    
    struct objc_super { id receiver; Class super_class; };
    
    void define_root_classes() {
        Class nsobject = mm_objc_define_class("NSObject", nullptr, sizeof(mm_objc_object), MM_OBJC_CLASS_REF(NSObject), MM_OBJC_METACLASS_REF(NSObject));
        struct { const char *sel; void *imp; const char *types; bool cls; } table[] = {
            {"alloc", reinterpret_cast<void *>(&NSObject_alloc), "@16@0:8", true},
            {"allocWithZone:", reinterpret_cast<void *>(&NSObject_alloc), "@24@0:8^v16", true},
            {"new", reinterpret_cast<void *>(&NSObject_new), "@16@0:8", true},
            {"class", reinterpret_cast<void *>(&NSObject_classClass), "#16@0:8", true},
            {"superclass", reinterpret_cast<void *>(&NSObject_classSuperclass), "#16@0:8", true},
            {"isSubclassOfClass:", reinterpret_cast<void *>(&NSObject_classIsSubclassOfClass), "B24@0:8#16", true},
            {"instancesRespondToSelector:", reinterpret_cast<void *>(&NSObject_instancesRespondToSelector), "B24@0:8:16", true},
            {"conformsToProtocol:", reinterpret_cast<void *>(&NSObject_conformsToProtocol), "B24@0:8@16", true},
            {"description", reinterpret_cast<void *>(&NSObject_classDescription), "@16@0:8", true},
            {"initialize", reinterpret_cast<void *>(&NSObject_initialize), "v16@0:8", true},
            {"load", reinterpret_cast<void *>(&NSObject_load), "v16@0:8", true},
            {"self", reinterpret_cast<void *>(&NSObject_self), "@16@0:8", true},
            {"retain", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
            /* A class object is immortal, so copying one yields the class. */
            {"copy", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
            {"copyWithZone:", reinterpret_cast<void *>(&NSObject_noop), "@24@0:8^v16", true},
            {"mutableCopy", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
            {"mutableCopyWithZone:", reinterpret_cast<void *>(&NSObject_noop), "@24@0:8^v16", true},
            {"hash", reinterpret_cast<void *>(&NSObject_hash), "Q16@0:8", true},
            {"isEqual:", reinterpret_cast<void *>(&NSObject_isEqual), "B24@0:8@16", true},
            {"release", reinterpret_cast<void *>(&NSObject_zone), "v16@0:8", true},
            {"autorelease", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
            {"respondsToSelector:", reinterpret_cast<void *>(&NSObject_respondsToSelector), "B24@0:8:16", true},
    mm_id mm_objc_retain(mm_id obj) { return do_retain(obj); }
    void mm_objc_release(mm_id obj) { do_release(obj); }
    mm_id mm_objc_autorelease(mm_id obj) { return obj ? reinterpret_cast<mm_id>(NSObject_autorelease(obj, nullptr)) : nullptr; }
    mm_SEL mm_objc_sel_of(mm_SEL sel) { return sel_intern(sel); }
    const mm_host_api *mm_objc_host_api() { return g_api; }
    void mm_objc_return_words_hi(uint64_t x1) {
        if (mm_cpu_context *ctx = g_api ? g_api->current_context() : nullptr) {
            ctx->x[1] = x1;
            if (ctx->native_mode) { ctx->native_x[MM_RDX] = x1; ctx->native_hi_valid = 1; }
        }
    }
    void mm_objc_return_doubles_hi(double v2, double v3) {
        mm_cpu_context *ctx = g_api ? g_api->current_context() : nullptr;
        if (!ctx) return;
        std::memcpy(&ctx->v[2][0], &v2, 8); ctx->v[2][1] = 0;
        std::memcpy(&ctx->v[3][0], &v3, 8); ctx->v[3][1] = 0;
    }
    uint64_t mm_objc_block_invoke(void *block, int nargs, const uint64_t *args) {
        auto *b = static_cast<block_layout *>(block);
        std::uint64_t a[8] = {reinterpret_cast<std::uintptr_t>(block)};
        for (int i = 0; i < nargs && i < 7; i++) a[1 + i] = args[i];
        if (is_guest(reinterpret_cast<const void *>(b->invoke)))
            return g_api->call_guest(reinterpret_cast<std::uintptr_t>(b->invoke), a, 1 + nargs);
        using fn8 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        return reinterpret_cast<fn8>(reinterpret_cast<void *>(b->invoke))(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    }
    
    } // extern "C"
    419:__attribute__((visibility("default"))) mm_objc_class mm_block_stack_class __asm__("__NSConcreteStackBlock");
    420:__attribute__((visibility("default"))) mm_objc_class mm_block_malloc_class __asm__("__NSConcreteMallocBlock");
    421:__attribute__((visibility("default"))) mm_objc_class mm_block_global_class __asm__("__NSConcreteGlobalBlock");
    768:struct block_descriptor { std::uintptr_t reserved, size; void (*copy)(void *, const void *); void (*dispose)(const void *); const char *signature; };
    769:struct block_layout { Class isa; std::int32_t flags; std::int32_t reserved; void (*invoke)(void *, ...); block_descriptor *descriptor; };
    770:struct block_byref { Class isa; block_byref *forwarding; std::int32_t flags; std::int32_t size; void (*keep)(block_byref *, block_byref *); void (*destroy)(block_byref *); };
    void darwin_dispatch_async(dispatch_queue *queue, void *block) DARWIN(dispatch_async);
    void darwin_dispatch_async(dispatch_queue *queue, void *block) {
        if (!block) return;
        if (dispatch_trace())
            std::fprintf(stderr, "dispatch: async on %s\n", (queue ? *queue : main_queue()).label.c_str());
        work_item item{retain(reinterpret_cast<id>(block)), nullptr, nullptr};
        (queue ? *queue : main_queue()).push(item);
    }
    
    void darwin_dispatch_async_f(dispatch_queue *queue, void *context, void *function) DARWIN(dispatch_async_f);
    void darwin_dispatch_async_f(dispatch_queue *queue, void *context, void *function) {
        if (!function) return;
        (queue ? *queue : main_queue()).push({nullptr, function, context});
    }
    
    /* dispatch_sync runs the block where the caller is: the work has to be
     * finished when it returns, and running it here is both the simplest way
     * to guarantee that and the one that cannot deadlock against the main
    struct error_object { mm_objc_object base; id domain = nullptr; std::int64_t code = 0; id user_info = nullptr; };
    error_object *err(id o) { return as<error_object>(o); }
    std::uint64_t err_domain(id self, SEL) { return ret(err(self)->domain); }
    std::uint64_t err_code(id self, SEL) { return static_cast<std::uint64_t>(err(self)->code); }
    std::uint64_t err_userInfo(id self, SEL) { return ret(err(self)->user_info ? err(self)->user_info : obj(send(reinterpret_cast<id>(cls("NSDictionary")), "dictionary"))); }
    std::uint64_t err_localizedDescription(id self, SEL) {
        if (err(self)->user_info)
            if (id d = dictionary_get(err(self)->user_info, NSLocalizedDescriptionKey_)) return ret(d);
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s error %lld.", describe(err(self)->domain).c_str(), static_cast<long long>(err(self)->code));
        return ret(autorelease(string_from_utf8(buf)));
    }
    std::uint64_t err_description(id self, SEL) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "Error Domain=%s Code=%lld", describe(err(self)->domain).c_str(), static_cast<long long>(err(self)->code));
        return ret(autorelease(string_from_utf8(buf)));
    }
    std::uint64_t err_initWithDomainCodeUserInfo(id self, SEL, id domain, std::uint64_t code, id info) {
        err(self)->domain = retain(domain);
        err(self)->code = static_cast<std::int64_t>(code);
        err(self)->user_info = info ? retain(info) : nullptr;
    ✓ • 0ms

## Activity

    file changes: Completed · 5 changes
    Update { move_path: None }: /mnt/media/machoman/CMakeLists.txt
    @@ -152,2 +152,3 @@
             src/dylibs/AppKit/stack_view.cpp
    +        src/dylibs/AppKit/webview.cpp
             src/dylibs/AppKit/pasteboard.cpp
    @@ -279,2 +280,4 @@
     
    +    add_subdirectory(modules/WebKit)
    +
         # Quartz re-exports AppKit, including NSApplicationMain.
    Add: /mnt/media/machoman/modules/WebKit/CMakeLists.txt
    # Cocoa's view payload stays in AppKit. The browser runs in a separate host
    # process, so Qt never shares a GUI loop, signals or TLS with the guest.
    option(MACHOMAN_BUILD_WEBKIT_BROWSER "Build the Qt WebEngine browser helper" ON)
    if(MACHOMAN_BUILD_WEBKIT_BROWSER)
        find_package(Qt6 QUIET COMPONENTS Widgets WebEngineWidgets WebChannel)
        if(Qt6_FOUND)
            add_executable(machoman-webview browser.cpp)
            set_target_properties(machoman-webview PROPERTIES AUTOMOC ON CXX_STANDARD 20)
            target_link_libraries(machoman-webview PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::WebChannel)
            target_compile_options(machoman-webview PRIVATE -Wall -Wextra)
            set_target_properties(machoman-webview PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libexec")
            target_compile_definitions(AppKit PRIVATE
                MM_WEBVIEW_HELPER="$<TARGET_FILE:machoman-webview>")
            add_dependencies(AppKit machoman-webview)
            install(TARGETS machoman-webview RUNTIME DESTINATION libexec)
        else()
            message(STATUS "Qt WebEngine unavailable: WKWebView will report a navigation error")
        endif()
    endif()
    
    # Re-export AppKit's WK classes through the actual WebKit install name.
    add_library(WebKit SHARED framework.cpp "${PROJECT_SOURCE_DIR}/src/dylibs/symbol-synthesis/synthesise.cpp")
    target_include_directories(WebKit PRIVATE "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/dylibs/symbol-synthesis")
    target_link_libraries(WebKit PRIVATE AppKit libobjc)
    target_compile_options(WebKit PRIVATE -Wall -Wextra -fvisibility=hidden)
    set(MM_WEBKIT_DIR "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/WebKit.framework/Versions/A")
    if(IS_SYMLINK "${MM_WEBKIT_DIR}/WebKit.so")
        file(REMOVE "${MM_WEBKIT_DIR}/WebKit.so")
    endif()
    set_target_properties(WebKit PROPERTIES PREFIX "" OUTPUT_NAME WebKit SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${MM_WEBKIT_DIR}"
        BUILD_RPATH "$<TARGET_FILE_DIR:AppKit>;${CMAKE_BINARY_DIR}/dylibs/usr/lib")
    Add: /mnt/media/machoman/modules/WebKit/framework.cpp
    #include "synthesise.h"
    extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
        return mm_synthesise_symbol(name, flags);
    }
    Update { move_path: None }: /mnt/media/machoman/src/dylibs/AppKit/appkit.h
    @@ -377,2 +377,3 @@
     void register_stack_view();  /* NSStackView container hierarchy */
    +void register_webview();     /* WKWebView shares the NSView object model */
     void register_pasteboard();
    Update { move_path: None }: /mnt/media/machoman/src/dylibs/AppKit/appkit_main.cpp
    @@ -62,2 +62,3 @@
         ak::register_stack_view();
    +    ak::register_webview();
         ak::register_application();

## User

кстати, а мы можем потом сделать чтобы qt webkit рисовался внутри окна?

## Assistant

Да. Qt WebEngine можно рисовать внутри области `WKWebView`: передавать кадры браузера в AppKit и направлять в него мышь и клавиатуру. Сейчас запускаю движок в отдельном процессе с собственным окном, чтобы довести вход и обратные вызовы; затем этот же процесс можно связать с отрисовкой внутри окна Roblox. Здесь используется Qt WebEngine на Chromium — Qt WebKit был другим движком.

## Activity

    file changes: Completed · 1 changes
    Add: /mnt/media/machoman/modules/WebKit/browser.cpp
    // One isolated Chromium view. stdin/stdout carry newline-delimited JSON;
    // diagnostics use stderr. Cookie values and page bodies are never logged.
    #include <QApplication>
    #include <QDateTime>
    #include <QFile>
    #include <QJsonArray>
    #include <QJsonDocument>
    #include <QJsonObject>
    #include <QNetworkCookie>
    #include <QSocketNotifier>
    #include <QWebChannel>
    #include <QWebEngineCookieStore>
    #include <QWebEngineHttpRequest>
    #include <QWebEngineLoadingInfo>
    #include <QWebEnginePage>
    #include <QWebEngineProfile>
    #include <QWebEngineScript>
    #include <QWebEngineScriptCollection>
    #include <QWebEngineSettings>
    #include <QWebEngineView>
    #include <cerrno>
    #include <sys/socket.h>
    #include <unistd.h>
    
    static void emit_event(QJsonObject event) {
        QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
        const char *p = bytes.constData();
        qsizetype left = bytes.size();
        while (left > 0) {
            const auto n = ::send(STDOUT_FILENO, p, size_t(left), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { QCoreApplication::quit(); return; }
            p += n; left -= n;
        }
    }
    class ScriptBridge : public QObject {
        Q_OBJECT
    public:
        using QObject::QObject;
        Q_INVOKABLE void postMessage(const QString &name, const QVariant &body) {
            emit_event({{"event", "message"}, {"name", name}, {"body", QJsonValue::fromVariant(body)}});
        }
    };
    
    static QJsonObject cookie_json(const QNetworkCookie &cookie) {
        QJsonObject out{{"Name", QString::fromUtf8(cookie.name())}, {"Value", QString::fromUtf8(cookie.value())},
            {"Domain", cookie.domain()}, {"Path", cookie.path()}, {"Secure", cookie.isSecure()},
            {"HTTPOnly", cookie.isHttpOnly()}};
        if (!cookie.isSessionCookie()) out.insert("Expires", double(cookie.expirationDate().toSecsSinceEpoch()));
        return out;
    }
    static QNetworkCookie read_cookie(const QJsonObject &o) {
        QNetworkCookie c(o["Name"].toString().toUtf8(), o["Value"].toString().toUtf8());
        c.setDomain(o["Domain"].toString()); c.setPath(o["Path"].toString());
        c.setSecure(o["Secure"].toBool()); c.setHttpOnly(o["HTTPOnly"].toBool());
        if (o.contains("Expires")) c.setExpirationDate(QDateTime::fromSecsSinceEpoch(qint64(o["Expires"].toDouble())));
        return c;
    }
    
    int main(int argc, char **argv) {
        QApplication app(argc, argv);
        app.setApplicationName("machoman-webview");
        QWebEngineProfile profile; // in-memory storage, explicitly seeded by Cocoa
        QWebEngineView view;
        auto *page = new QWebEnginePage(&profile, &view);
        view.setPage(page);
        view.setWindowTitle("Web content");
        view.resize(640, 600);
        QWebChannel channel(page);
        ScriptBridge bridge;
        channel.registerObject("machoman", &bridge);
        page->setWebChannel(&channel, QWebEngineScript::MainWorld);
        QObject::connect(page, &QWebEnginePage::loadStarted, [&] { emit_event({{"event", "started"}}); });
        QObject::connect(page, &QWebEnginePage::loadFinished, [&](bool ok) {
            emit_event({{"event", "finished"}, {"ok", ok}, {"url", page->url().toString()}});
        });
        QObject::connect(page, &QWebEnginePage::loadingChanged, [&](const QWebEngineLoadingInfo &info) {
            if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus)
                emit_event({{"event", "error"}, {"code", info.errorCode()}, {"message", info.errorString()}});
        });
        QObject::connect(page, &QWebEnginePage::urlChanged, [&](const QUrl &url) {
            emit_event({{"event", "url"}, {"url", url.toString()}});
        });
        QObject::connect(page, &QWebEnginePage::titleChanged, [&](const QString &title) {
            view.setWindowTitle(title.isEmpty() ? "Web content" : title);
            emit_event({{"event", "title"}, {"title", title}});
        });
        QObject::connect(profile.cookieStore(), &QWebEngineCookieStore::cookieAdded, [&](const QNetworkCookie &cookie) {
            emit_event({{"event", "cookie"}, {"cookie", cookie_json(cookie)}});
        });
        QObject::connect(profile.cookieStore(), &QWebEngineCookieStore::cookieRemoved, [&](const QNetworkCookie &cookie) {
            emit_event({{"event", "cookieRemoved"}, {"cookie", cookie_json(cookie)}});
        });
        QFile channel_source(":/qtwebchannel/qwebchannel.js");
        channel_source.open(QIODevice::ReadOnly);
        const QString channel_js = QString::fromUtf8(channel_source.readAll());
        auto configure = [&](const QJsonObject &o) {
            const QString suffix = o["applicationName"].toString();
            const QString custom = o["userAgent"].toString();
            if (!custom.isEmpty()) profile.setHttpUserAgent(custom);
            else if (!suffix.isEmpty()) profile.setHttpUserAgent(profile.httpUserAgent() + ' ' + suffix);
            const auto cookies = o["cookies"].toArray();
            for (const auto &c : cookies) profile.cookieStore()->setCookie(read_cookie(c.toObject()));
            page->scripts().clear();
            // Install handlers immediately and queue posts until the channel is ready.
            QString js = channel_js + QStringLiteral(R"JS(
                (function() {
                  const pending = []; let bridge = null;
                  window.webkit = window.webkit || {};
                  window.webkit.messageHandlers = window.webkit.messageHandlers || {};
                  const post = (name, body) => {
                    if (bridge) bridge.postMessage(name, body);
                    else pending.push([name, body]);
                  };
            )JS");
            for (const auto &name : o["handlers"].toArray()) {
                const QString literal = QString::fromUtf8(QJsonDocument(QJsonArray{name}).toJson(QJsonDocument::Compact));
                js += "{ const name = " + literal + "[0]; window.webkit.messageHandlers[name] = {postMessage: body => post(name, body)}; }\n";
            }
            js += QStringLiteral(R"JS(
                  if (typeof qt !== 'undefined' && qt.webChannelTransport) {
                    new QWebChannel(qt.webChannelTransport, channel => {
                      bridge = channel.objects.machoman;
                      for (const args of pending) bridge.postMessage(args[0], args[1]);
                    });
                  }
                })();
            )JS");
            QWebEngineScript bootstrap;
            bootstrap.setName("Cocoa script message handlers");
            bootstrap.setInjectionPoint(QWebEngineScript::DocumentCreation);
            bootstrap.setWorldId(QWebEngineScript::MainWorld);
            bootstrap.setRunsOnSubFrames(true); bootstrap.setSourceCode(js);
            page->scripts().insert(bootstrap);
            for (const auto &value : o["scripts"].toArray()) {
                const auto script = value.toObject();
                QWebEngineScript s;
                s.setSourceCode(script["source"].toString());
                s.setInjectionPoint(script["time"].toInt() == 0 ? QWebEngineScript::DocumentCreation : QWebEngineScript::DocumentReady);
                s.setWorldId(QWebEngineScript::MainWorld);
                s.setRunsOnSubFrames(!script["mainOnly"].toBool()); page->scripts().insert(s);
            }
        };
        QByteArray input;
        QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&notifier, &QSocketNotifier::activated, [&](QSocketDescriptor, QSocketNotifier::Type) {
            char bytes[8192];
            const auto n = ::recv(STDIN_FILENO, bytes, sizeof bytes, 0);
            if (n <= 0) { notifier.setEnabled(false); app.quit(); return; }
            input.append(bytes, qsizetype(n));
            if (input.size() > 8 * 1024 * 1024) { app.quit(); return; }
            qsizetype end;
            while ((end = input.indexOf('\n')) >= 0) {
                const QByteArray line = input.left(end); input.remove(0, end + 1);
                const auto o = QJsonDocument::fromJson(line).object();
                const QString op = o["op"].toString();
                if (op == "quit") { app.quit(); return; }
                if (op == "configure") configure(o);
                else if (op == "open") {
                    QWebEngineHttpRequest request(QUrl(o["url"].toString()));
                    if (o["method"].toString() == "POST") {
                        request.setMethod(QWebEngineHttpRequest::Post);
                        request.setPostData(QByteArray::fromBase64(o["body"].toString().toUtf8()));
                    }
                    const auto headers = o["headers"].toObject();
                    for (auto it = headers.begin(); it != headers.end(); ++it)
                        request.setHeader(it.key().toUtf8(), it.value().toString().toUtf8());
                    view.show(); page->load(request);
                } else if (op == "html") { view.show(); page->setHtml(o["html"].toString(), QUrl(o["base"].toString())); }
                else if (op == "js") {
                    const QJsonValue id = o["id"];
                    page->runJavaScript(o["source"].toString(), QWebEngineScript::MainWorld, [id](const QVariant &result) {
                        emit_event({{"event", "js"}, {"id", id}, {"result", QJsonValue::fromVariant(result)}});
                    });
                } else if (op == "stop") page->triggerAction(QWebEnginePage::Stop);
                else if (op == "reload") page->triggerAction(QWebEnginePage::Reload);
                else if (op == "back") page->triggerAction(QWebEnginePage::Back);
                else if (op == "forward") page->triggerAction(QWebEnginePage::Forward);
                else if (op == "resize") view.resize(std::max(100, o["width"].toInt()), std::max(100, o["height"].toInt()));
                else if (op == "hide") view.hide();
                else if (op == "cookie") profile.cookieStore()->setCookie(read_cookie(o["cookie"].toObject()));
                else if (op == "deleteCookie") profile.cookieStore()->deleteCookie(read_cookie(o["cookie"].toObject()));
            }
        });
        emit_event({{"event", "ready"}});
        return app.exec();
    }
    #include "browser.moc"

## Activity

    file changes: Completed · 1 changes
    Add: /mnt/media/machoman/src/dylibs/AppKit/webview.cpp
    /* WKWebView is a real NSView. Chromium lives in an isolated host helper;
     * only the main Cocoa queue sends messages back into guest delegates. */
    #include "appkit.h"
    #include "machoman/json.h"
    #include <algorithm>
    #include <atomic>
    #include <cerrno>
    #include <chrono>
    #include <cmath>
    #include <dlfcn.h>
    #include <filesystem>
    #include <memory>
    #include <mutex>
    #include <spawn.h>
    #include <sys/socket.h>
    #include <sys/wait.h>
    #include <thread>
    #include <unistd.h>
    
    extern char **environ;
    MM_FW_CLASS(WKWebView); MM_FW_CLASS(WKWebViewConfiguration);
    MM_FW_CLASS(WKWebsiteDataStore); MM_FW_CLASS(WKHTTPCookieStore);
    MM_FW_CLASS(WKUserContentController); MM_FW_CLASS(WKUserScript);
    MM_FW_CLASS(WKScriptMessage); MM_FW_CLASS(WKNavigation); MM_FW_CLASS(WKPreferences);
    
    namespace ak {
    namespace {
    using json_object = mm_json::object;
    using json_array = mm_json::array;
    mm_json num(std::uint64_t n) { return mm_json(mm_json_number{std::to_string(n)}); }
    const mm_json &field(const mm_json &o, const char *key) {
        static const mm_json missing;
        const auto *p = std::get_if<json_object>(&o.value);
        if (!p) return missing;
        auto it = p->find(key); return it == p->end() ? missing : it->second;
    }
    std::string text(const mm_json &j) {
        const auto *p = std::get_if<std::string>(&j.value); return p ? *p : std::string();
    }
    std::uint64_t integer(const mm_json &j) {
        const auto *n = std::get_if<mm_json_number>(&j.value);
        return n ? std::strtoull(n->text.c_str(), nullptr, 10) : 0;
    }
    bool truth(const mm_json &j) { const auto *b = std::get_if<bool>(&j.value); return b && *b; }
    id array() { return obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array")); }
    id dictionary() { return obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary")); }
    void dictionary_put(id d, const char *key, id value) {
        if (value) send(d, "setObject:forKey:", ret(value), ret(string(key)));
    }
    id from_json(const mm_json &j) {
        if (const auto *s = std::get_if<std::string>(&j.value)) return string(*s);
        if (const auto *b = std::get_if<bool>(&j.value))
            return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", *b));
        if (const auto *n = std::get_if<mm_json_number>(&j.value)) {
            if (n->text.find_first_of(".eE") != std::string::npos) return number(std::strtod(n->text.c_str(), nullptr));
            return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithLongLong:", std::strtoull(n->text.c_str(), nullptr, 10)));
        }
        if (const auto *a = std::get_if<json_array>(&j.value)) {
            id out = array(); for (const auto &v : *a) send(out, "addObject:", ret(from_json(v))); return out;
        }
        if (const auto *o = std::get_if<json_object>(&j.value)) {
            id out = dictionary();
            for (const auto &[key, v] : *o) send(out, "setObject:forKey:", ret(from_json(v)), ret(string(key)));
            return out;
        }
        return obj(send(reinterpret_cast<id>(cls("NSNull")), "null"));
    }
    id error(std::int64_t code, const std::string &description) {
        id info = dictionary(); dictionary_put(info, "NSLocalizedDescription", string(description));
        return obj(send(reinterpret_cast<id>(cls("NSError")), "errorWithDomain:code:userInfo:",
            ret(string("WKErrorDomain")), static_cast<std::uint64_t>(code), ret(info)));
    }
    struct object {
        mm_objc_object base;
        std::map<std::string, id> values;
        ~object() { for (const auto &[key, value] : values) release(value); }
    };
    id get(id self, const char *key) {
        auto &values = as<object>(self)->values;
        const auto it = values.find(key); return it == values.end() ? nullptr : it->second;
    }
    void put(id self, const char *key, id value) {
        auto &slot = as<object>(self)->values[key];
        retain(value); release(slot); slot = value;
    }
    id make_object(const char *name) { return obj(send(reinterpret_cast<id>(cls(name)), "new")); }
    std::uint64_t copy_object(id self, SEL, void *) {
        id copy = make_object(mm_objc_class_name(self->isa));
        for (const auto &[key, value] : as<object>(self)->values) put(copy, key.c_str(), value);
        return ret(copy);
    }
    void property(Class c, const char *key, const char *setter) {
        // Accessors use their actual selector as the key, and derive the setter key.
        mm_objc_add_method(c, key, MM_IMP(+[](id s, SEL sel) { return ret(get(s, sel)); }), "@16@0:8");
        mm_objc_add_method(c, setter, MM_IMP(+[](id s, SEL sel, id value) -> std::uint64_t {
            std::string key(sel + 3); key.pop_back();
            key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
            put(s, key.c_str(), value); return 0;
        }), "v24@0:8@16");
    }
    Class plain(const char *name, Class *storage, Class *meta) {
        Class c = mm_objc_define_class(name, cls("NSObject"), sizeof(object), storage, meta);
        register_lifecycle<object>(c);
        MM_METHODS(c, {"copyWithZone:", MM_IMP(copy_object), "@24@0:8^v16"});
        return c;
    }
    
    using dispatch_fn = void (*)(void *, void *, void *);
    using main_queue_fn = void *(*)();
    dispatch_fn dispatch = nullptr;
    main_queue_fn main_queue = nullptr;
    struct task { void (*run)(task *); virtual ~task() = default; };
    void run_task(std::uint64_t pointer) {
        std::unique_ptr<task> item(reinterpret_cast<task *>(pointer));
        autorelease_pool pool; item->run(item.get());
    }
    void post(task *item) {
        if (dispatch && main_queue) dispatch(main_queue(), item, reinterpret_cast<void *>(run_task));
        else { std::fprintf(stderr, "WebKit: Cocoa main queue is unavailable\n"); delete item; }
    }
    struct block_task : task {
        id block; std::vector<id> objects;
        block_task(id b, std::initializer_list<id> args) : block(retain(b)), objects(args) {
            for (id value : objects) retain(value);
            run = +[](task *t) {
                auto *self = static_cast<block_task *>(t);
                std::vector<std::uint64_t> words;
                for (id value : self->objects) words.push_back(ret(value));
                mm_objc_block_invoke(self->block, static_cast<int>(words.size()), words.data());
            };
        }
        ~block_task() { release(block); for (id value : objects) release(value); }
    };
    void complete(id block, std::initializer_list<id> args = {}) { if (block) post(new block_task(block, args)); }
    
    struct cookie_store {
        mm_objc_object base;
        std::vector<id> cookies;
        bool persistent = false;
        ~cookie_store() { for (id c : cookies) release(c); }
    };
    id cocoa_cookies() { return obj(send(reinterpret_cast<id>(cls("NSHTTPCookieStorage")), "sharedHTTPCookieStorage")); }
    bool same_cookie(id a, id b) {
        for (const char *key : {"name", "domain", "path"})
            if (to_utf8(obj(send(a, key))) != to_utf8(obj(send(b, key)))) return false;
        return true;
    }
    void save_cookie(id store, id cookie, bool remove) {
        if (!cookie) return;
        auto *s = as<cookie_store>(store);
        auto it = std::find_if(s->cookies.begin(), s->cookies.end(), [&](id c) { return same_cookie(c, cookie); });
        if (it != s->cookies.end()) { release(*it); s->cookies.erase(it); }
        if (!remove) s->cookies.push_back(retain(cookie));
        if (s->persistent) send(cocoa_cookies(), remove ? "deleteCookie:" : "setCookie:", ret(cookie));
    }
    mm_json encode_cookie(id cookie) {
        json_object out;
        for (const auto &[property, key] : {std::pair{"name", "Name"}, {"value", "Value"},
            {"domain", "Domain"}, {"path", "Path"}})
            out[key] = mm_json(to_utf8(obj(send(cookie, property))));
        out["Secure"] = mm_json(send(cookie, "isSecure") != 0);
        out["HTTPOnly"] = mm_json(send(cookie, "isHTTPOnly") != 0);
        if (id date = obj(send(cookie, "expiresDate"))) {
            // NSDate's interval is a floating return; read it from the runtime context.
            send(date, "timeIntervalSince1970");
            if (auto *ctx = api()->current_context()) {
                double time; std::memcpy(&time, &ctx->v[0][0], 8);
                out["Expires"] = mm_json(mm_json_number{std::to_string(time)});
            }
        }
        return mm_json(std::move(out));
    }
    id decode_cookie(const mm_json &value) {
        id props = dictionary();
        for (const char *key : {"Name", "Value", "Domain", "Path", "Secure", "HTTPOnly"})
            dictionary_put(props, key, from_json(field(value, key)));
        if (std::get_if<mm_json_number>(&field(value, "Expires").value)) {
            double time = std::strtod(std::get<mm_json_number>(field(value, "Expires").value).text.c_str(), nullptr);
            if (auto *ctx = api()->current_context()) std::memcpy(&ctx->v[0][0], &time, 8);
            dictionary_put(props, "Expires", obj(send(reinterpret_cast<id>(cls("NSDate")), "dateWithTimeIntervalSince1970:")));
        }
        return obj(send(reinterpret_cast<id>(cls("NSHTTPCookie")), "cookieWithProperties:", ret(props)));
    }
    id default_store = nullptr;
    id data_store(bool persistent) {
        if (persistent && default_store) return default_store;
        id store = make_object("WKWebsiteDataStore");
        id cookies = make_object("WKHTTPCookieStore");
        as<cookie_store>(cookies)->persistent = persistent;
        if (persistent) {
            id all = obj(send(cocoa_cookies(), "cookies"));
            for (std::uint64_t i = 0, n = send(all, "count"); i < n; ++i)
                as<cookie_store>(cookies)->cookies.push_back(retain(obj(send(all, "objectAtIndex:", i))));
        }
        put(store, "httpCookieStore", cookies); release(cookies);
        put(store, "persistent", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", persistent)));
        if (persistent) default_store = store;
        else autorelease(store);
        return store;
    }
    id config_child(id config, const char *key, const char *type) {
        id child = get(config, key);
        if (!child) {
            child = make_object(type); put(config, key, child); release(child);
        }
        return child;
    }
    id config_store(id config) {
        id store = get(config, "websiteDataStore");
        if (!store) { store = data_store(true); put(config, "websiteDataStore", store); }
        return store;
    }
    
    struct browser;
    struct webview {
        view_object view;
        id configuration = nullptr, navigation_delegate = nullptr, ui_delegate = nullptr;
        id navigation = nullptr, url = nullptr, title = nullptr;
        bool loading = false;
        std::shared_ptr<browser> process;
        std::map<std::uint64_t, id> completions;
        std::uint64_t serial = 0;
        ~webview();
    };
    struct event_task;
    struct browser : std::enable_shared_from_this<browser> {
        id owner = nullptr; // main queue only, cleared before the view is destroyed
        pid_t pid = -1;
        int socket = -1;
        std::thread reader;
        std::mutex write_mutex;
        std::atomic<bool> stopping = false;
        bool send_json(const mm_json &command) {
            const std::string line = mm_json_serialize(command) + '\n';
            std::lock_guard lock(write_mutex);
            std::size_t written = 0;
            while (socket >= 0 && written < line.size()) {
                const auto n = ::send(socket, line.data() + written, line.size() - written, MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                written += static_cast<std::size_t>(n);
            }
            return written == line.size();
        }
        void stop() {
            if (stopping.exchange(true)) return;
            owner = nullptr;
            send_json(mm_json(json_object{{"op", mm_json(std::string("quit"))}}));
            {
                std::lock_guard lock(write_mutex);
                if (socket >= 0) ::shutdown(socket, SHUT_RDWR);
            }
            if (reader.joinable()) reader.join();
            if (socket >= 0) { ::close(socket); socket = -1; }
            if (pid > 0) {
                int status;
                for (int i = 0; i < 20; ++i) {
                    if (::waitpid(pid, &status, WNOHANG) != 0) { pid = -1; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, &status, 0); pid = -1; }
            }
        }
        ~browser() { stop(); }
        void read_events();
    };
    std::vector<std::weak_ptr<browser>> browsers;
    webview::~webview() {
        if (process) process->stop();
        release(configuration); release(navigation); release(url); release(title);
        for (auto &[key, block] : completions) release(block);
    }
    id view_cookie_store(webview *v) { return get(config_store(v->configuration), "httpCookieStore"); }
    void notify(webview *v, id self, const char *selector, id arg) {
        if (responds(v->navigation_delegate, selector))
            send(v->navigation_delegate, selector, ret(self), ret(arg));
    }
    void navigation_error(id self, id e) {
        auto *v = as<webview>(self); v->loading = false;
        if (responds(v->navigation_delegate, "webView:didFailProvisionalNavigation:withError:"))
            send(v->navigation_delegate, "webView:didFailProvisionalNavigation:withError:", ret(self), ret(v->navigation), ret(e));
        std::fprintf(stderr, "WebKit: navigation failed: %s\n", to_utf8(obj(send(e, "localizedDescription"))).c_str());
    }
    void handle_event(const std::shared_ptr<browser> &process, const mm_json &event) {
        id self = retain(process->owner);
        if (!self) return;
        auto *v = as<webview>(self);
        const std::string name = text(field(event, "event"));
        if (name == "started") { v->loading = true; notify(v, self, "webView:didStartProvisionalNavigation:", v->navigation); }
        else if (name == "finished") {
            v->loading = false;
            if (truth(field(event, "ok"))) {
                notify(v, self, "webView:didCommitNavigation:", v->navigation);
                notify(v, self, "webView:didFinishNavigation:", v->navigation);
            }
        } else if (name == "url") {
            id value = obj(send(reinterpret_cast<id>(cls("NSURL")), "URLWithString:", ret(string(text(field(event, "url"))))));
            retain(value); release(v->url); v->url = value;
        } else if (name == "title") { id value = retain(string(text(field(event, "title")))); release(v->title); v->title = value; }
        else if (name == "error" || name == "closed")
            navigation_error(self, error(1, name == "closed" ? "Browser helper exited" : text(field(event, "message"))));
        else if (name == "js") {
            auto it = v->completions.find(integer(field(event, "id")));
            if (it != v->completions.end()) {
                id block = it->second; v->completions.erase(it);
                const std::uint64_t args[] = {ret(from_json(field(event, "result"))), 0};
                mm_objc_block_invoke(block, 2, args); release(block);
            }
        } else if (name == "message") {
            id controller = config_child(v->configuration, "userContentController", "WKUserContentController");
            const std::string handler_name = text(field(event, "name"));
            id handler = get(controller, ("handler:" + handler_name).c_str());
            if (responds(handler, "userContentController:didReceiveScriptMessage:")) {
                id message = autorelease(make_object("WKScriptMessage"));
                put(message, "name", string(handler_name)); put(message, "body", from_json(field(event, "body")));
                put(message, "webView", self);
                send(handler, "userContentController:didReceiveScriptMessage:", ret(controller), ret(message));
            }
        } else if (name == "cookie" || name == "cookieRemoved")
            save_cookie(view_cookie_store(v), decode_cookie(field(event, "cookie")), name == "cookieRemoved");
        release(self);
    }
    struct event_task : task {
        std::shared_ptr<browser> process; mm_json event;
        event_task(std::shared_ptr<browser> p, mm_json e) : process(std::move(p)), event(std::move(e)) {
            run = +[](task *t) { auto *e = static_cast<event_task *>(t); handle_event(e->process, e->event); };
        }
    };
    void browser::read_events() {
        char bytes[8192]; std::string pending;
        while (!stopping) {
            const auto n = ::recv(socket, bytes, sizeof bytes, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            pending.append(bytes, static_cast<std::size_t>(n));
            if (pending.size() > 8 * 1024 * 1024) break;
            std::size_t end;
            while ((end = pending.find('\n')) != std::string::npos) {
                mm_json event; std::string diagnostic;
                if (mm_json_parse(std::string_view(pending).substr(0, end), event, diagnostic))
                    post(new event_task(shared_from_this(), std::move(event)));
                pending.erase(0, end + 1);
            }
        }
        if (!stopping) post(new event_task(shared_from_this(), mm_json(json_object{{"event", mm_json(std::string("closed"))}})));
    }
    json_object browser_configuration(webview *v) {
        json_object out{{"op", mm_json(std::string("configure"))}};
        out["applicationName"] = mm_json(to_utf8(get(v->configuration, "applicationNameForUserAgent")));
        out["userAgent"] = mm_json(to_utf8(get(v->configuration, "customUserAgent")));
        json_array cookies;
        for (id cookie : as<cookie_store>(view_cookie_store(v))->cookies) cookies.push_back(encode_cookie(cookie));
        out["cookies"] = mm_json(std::move(cookies));
        id controller = config_child(v->configuration, "userContentController", "WKUserContentController");
        json_array handlers, scripts;
        for (const auto &[key, value] : as<object>(controller)->values)
            if (key.starts_with("handler:")) handlers.emplace_back(key.substr(8));
        if (id array = get(controller, "userScripts"))
            for (std::uint64_t i = 0, n = send(array, "count"); i < n; ++i) {
                id s = obj(send(array, "objectAtIndex:", i));
                scripts.emplace_back(json_object{{"source", mm_json(to_utf8(get(s, "source")))},
                    {"time", num(send(get(s, "injectionTime"), "unsignedLongLongValue"))},
                    {"mainOnly", mm_json(send(get(s, "forMainFrameOnly"), "boolValue") != 0)}});
            }
        out["handlers"] = mm_json(std::move(handlers)); out["scripts"] = mm_json(std::move(scripts));
        return out;
    }
    bool ensure_browser(id self) {
        auto *v = as<webview>(self);
        if (v->process) return !v->process->stopping;
        std::filesystem::path helper;
        if (const char *override = std::getenv("MACHOMAN_WEBVIEW_HELPER")) helper = override;
    #ifdef MM_WEBVIEW_HELPER
        else helper = MM_WEBVIEW_HELPER;
    #endif
        if (helper.empty() || ::access(helper.c_str(), X_OK) != 0) {
            navigation_error(self, error(1, "Qt WebEngine browser helper is unavailable")); return false;
        }
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
            navigation_error(self, error(1, "Cannot create browser connection")); return false;
        }
        auto process = std::make_shared<browser>(); process->owner = self; process->socket = sockets[0];
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose(&actions, sockets[0]);
        ::posix_spawn_file_actions_addclose(&actions, sockets[1]);
        std::string path = helper.string(); char *argv[] = {path.data(), nullptr};
        const int result = ::posix_spawn(&process->pid, path.c_str(), &actions, nullptr, argv, environ);
        ::posix_spawn_file_actions_destroy(&actions); ::close(sockets[1]);
        if (result != 0) { navigation_error(self, error(1, "Cannot launch browser helper")); return false; }
        v->process = process; browsers.push_back(process);
        process->reader = std::thread([process] { process->read_events(); });
        process->send_json(mm_json(browser_configuration(v)));
        process->send_json(mm_json(json_object{{"op", mm_json(std::string("resize"))},
            {"width", num(static_cast<std::uint64_t>(std::max(100., v->view.frame.w)))},
            {"height", num(static_cast<std::uint64_t>(std::max(100., v->view.frame.h)))}}));
        std::fprintf(stderr, "WebKit: browser helper started (PID %d)\n", process->pid);
        return true;
    }
    std::uint64_t view_init(id self, SEL, id configuration, double x, double y, double width, double height) {
        auto *v = as<webview>(self);
        v->view.frame = {x, y, width, height}; v->view.bounds = {0, 0, width, height};
        release(v->configuration);
        v->configuration = configuration ? obj(send(configuration, "copy")) : make_object("WKWebViewConfiguration");
        return ret(self);
    }
    void new_navigation(webview *v) {
        release(v->navigation); v->navigation = make_object("WKNavigation"); v->loading = true;
    }
    std::uint64_t load_request(id self, SEL, id request) {
        auto *v = as<webview>(self); new_navigation(v);
        id url = obj(send(request, "URL"));
        retain(url); release(v->url); v->url = url;
        if (!ensure_browser(self)) return 0;
        json_object command{{"op", mm_json(std::string("open"))},
            {"url", mm_json(to_utf8(obj(send(url, "absoluteString"))))},
            {"method", mm_json(to_utf8(obj(send(request, "HTTPMethod"))))}};
        id body = obj(send(request, "HTTPBody"));
        if (body) command["body"] = mm_json(to_utf8(obj(send(body, "base64EncodedStringWithOptions:", 0))));
        json_object headers;
        id source = obj(send(request, "allHTTPHeaderFields")), keys = obj(send(source, "allKeys"));
        for (std::uint64_t i = 0, n = send(keys, "count"); i < n; ++i) {
            id key = obj(send(keys, "objectAtIndex:", i));
            headers[to_utf8(key)] = mm_json(to_utf8(obj(send(source, "objectForKey:", ret(key)))));
        }
        command["headers"] = mm_json(std::move(headers));
        if (!v->process->send_json(mm_json(std::move(command)))) navigation_error(self, error(1, "Browser connection closed"));
        return ret(autorelease(retain(v->navigation)));
    }
    std::uint64_t load_html(id self, SEL, id html, id base) {
        auto *v = as<webview>(self); new_navigation(v);
        if (!ensure_browser(self)) return 0;
        v->process->send_json(mm_json(json_object{{"op", mm_json(std::string("html"))}, {"html", mm_json(to_utf8(html))},
            {"base", mm_json(to_utf8(obj(send(base, "absoluteString"))))}}));
        return ret(autorelease(retain(v->navigation)));
    }
    std::uint64_t evaluate(id self, SEL, id source, id block) {
        auto *v = as<webview>(self);
        if (!ensure_browser(self)) { complete(block, {nullptr, error(1, "Browser unavailable")}); return 0; }
        const auto id = ++v->serial;
        if (block) v->completions[id] = retain(block);
        if (!v->process->send_json(mm_json(json_object{{"op", mm_json(std::string("js"))},
            {"id", num(id)}, {"source", mm_json(to_utf8(source))}}))) {
            if (block) { release(v->completions[id]); v->completions.erase(id); }
            complete(block, {nullptr, error(1, "Browser connection closed")});
        }
        return 0;
    }
    std::uint64_t browser_action(id self, SEL sel) {
        auto *v = as<webview>(self);
        std::string op = std::strcmp(sel, "stopLoading") == 0 ? "stop" :
            std::strcmp(sel, "goBack") == 0 ? "back" : std::strcmp(sel, "goForward") == 0 ? "forward" : "reload";
        if (v->process) v->process->send_json(mm_json(json_object{{"op", mm_json(op)}}));
        return op == "stop" ? 0 : ret(v->navigation);
    }
    std::uint64_t cookie_change(id self, SEL sel, id cookie, id handler) {
        const bool remove = std::strcmp(sel, "deleteCookie:completionHandler:") == 0;
        save_cookie(self, cookie, remove);
        for (const auto &weak : browsers) if (auto browser = weak.lock()) {
            if (browser->owner && view_cookie_store(as<webview>(browser->owner)) == self)
                browser->send_json(mm_json(json_object{{"op", mm_json(std::string(remove ? "deleteCookie" : "cookie"))},
                    {"cookie", encode_cookie(cookie)}}));
        }
        complete(handler); return 0;
    }
    } // namespace
    
    void register_webview() {
        dispatch = reinterpret_cast<dispatch_fn>(dlsym(RTLD_DEFAULT, "_dispatch_async_f"));
        main_queue = reinterpret_cast<main_queue_fn>(dlsym(RTLD_DEFAULT, "_dispatch_get_main_queue"));
        Class config = plain("WKWebViewConfiguration", MM_FW_CLASS_REF(WKWebViewConfiguration), MM_FW_META_REF(WKWebViewConfiguration));
        Class store = plain("WKWebsiteDataStore", MM_FW_CLASS_REF(WKWebsiteDataStore), MM_FW_META_REF(WKWebsiteDataStore));
        Class controller = plain("WKUserContentController", MM_FW_CLASS_REF(WKUserContentController), MM_FW_META_REF(WKUserContentController));
        Class script = plain("WKUserScript", MM_FW_CLASS_REF(WKUserScript), MM_FW_META_REF(WKUserScript));
        Class message = plain("WKScriptMessage", MM_FW_CLASS_REF(WKScriptMessage), MM_FW_META_REF(WKScriptMessage));
        plain("WKNavigation", MM_FW_CLASS_REF(WKNavigation), MM_FW_META_REF(WKNavigation));
        Class preferences = plain("WKPreferences", MM_FW_CLASS_REF(WKPreferences), MM_FW_META_REF(WKPreferences));
        Class cookies = mm_objc_define_class("WKHTTPCookieStore", cls("NSObject"), sizeof(cookie_store),
            MM_FW_CLASS_REF(WKHTTPCookieStore), MM_FW_META_REF(WKHTTPCookieStore));
        register_lifecycle<cookie_store>(cookies);
        Class view = mm_objc_define_class("WKWebView", cls("NSView"), sizeof(webview), MM_FW_CLASS_REF(WKWebView), MM_FW_META_REF(WKWebView));
        register_lifecycle<webview>(view);
        property(config, "applicationNameForUserAgent", "setApplicationNameForUserAgent:");
        property(config, "websiteDataStore", "setWebsiteDataStore:");
        property(config, "userContentController", "setUserContentController:");
        property(config, "preferences", "setPreferences:");
        MM_METHODS(config,
            {"websiteDataStore", MM_IMP(+[](id s, SEL) { return ret(config_store(s)); }), "@16@0:8"},
            {"userContentController", MM_IMP(+[](id s, SEL) { return ret(config_child(s, "userContentController", "WKUserContentController")); }), "@16@0:8"},
            {"preferences", MM_IMP(+[](id s, SEL) { return ret(config_child(s, "preferences", "WKPreferences")); }), "@16@0:8"});
        MM_CLASS_METHODS(store,
            {"defaultDataStore", MM_IMP(+[](id, SEL) { return ret(data_store(true)); }), "@16@0:8"},
            {"nonPersistentDataStore", MM_IMP(+[](id, SEL) { return ret(data_store(false)); }), "@16@0:8"});
        MM_METHODS(store,
            {"httpCookieStore", MM_IMP(+[](id s, SEL) { return ret(get(s, "httpCookieStore")); }), "@16@0:8"},
            {"isPersistent", MM_IMP(+[](id s, SEL) { return send(get(s, "persistent"), "boolValue"); }), "B16@0:8"});
        MM_METHODS(cookies,
            {"setCookie:completionHandler:", MM_IMP(cookie_change), "v32@0:8@16@?24"},
            {"deleteCookie:completionHandler:", MM_IMP(cookie_change), "v32@0:8@16@?24"},
            {"getAllCookies:", MM_IMP(+[](id s, SEL, id block) -> std::uint64_t {
                id all = array(); for (id c : as<cookie_store>(s)->cookies) send(all, "addObject:", ret(c));
                complete(block, {all}); return 0;
            }), "v24@0:8@?16"});
        MM_METHODS(controller,
            {"addScriptMessageHandler:name:", MM_IMP(+[](id s, SEL, id handler, id name) -> std::uint64_t {
                put(s, ("handler:" + to_utf8(name)).c_str(), handler); return 0;
            }), "v32@0:8@16@24"},
            {"removeScriptMessageHandlerForName:", MM_IMP(+[](id s, SEL, id name) -> std::uint64_t {
                put(s, ("handler:" + to_utf8(name)).c_str(), nullptr); return 0;
            }), "v24@0:8@16"},
            {"addUserScript:", MM_IMP(+[](id s, SEL, id value) -> std::uint64_t {
                id scripts = get(s, "userScripts"); if (!scripts) { scripts = array(); put(s, "userScripts", scripts); }
                send(scripts, "addObject:", ret(value)); return 0;
            }), "v24@0:8@16"},
            {"userScripts", MM_IMP(+[](id s, SEL) { return ret(get(s, "userScripts") ? get(s, "userScripts") : array()); }), "@16@0:8"},
            {"removeAllUserScripts", MM_IMP(+[](id s, SEL) -> std::uint64_t { put(s, "userScripts", nullptr); return 0; }), "v16@0:8"});
        MM_METHODS(script,
            {"initWithSource:injectionTime:forMainFrameOnly:", MM_IMP(+[](id s, SEL, id source, std::uint64_t time, std::uint64_t only) {
                put(s, "source", source);
                put(s, "injectionTime", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithUnsignedLongLong:", time)));
                put(s, "forMainFrameOnly", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", only)));
                return ret(s);
            }), "@40@0:8@16q24B32"},
            {"source", MM_IMP(+[](id s, SEL) { return ret(get(s, "source")); }), "@16@0:8"},
            {"injectionTime", MM_IMP(+[](id s, SEL) { return send(get(s, "injectionTime"), "unsignedLongLongValue"); }), "q16@0:8"},
            {"isForMainFrameOnly", MM_IMP(+[](id s, SEL) { return send(get(s, "forMainFrameOnly"), "boolValue"); }), "B16@0:8"});
        for (const char *key : {"name", "body", "webView"})
            mm_objc_add_method(message, key, MM_IMP(+[](id s, SEL sel) { return ret(get(s, sel)); }), "@16@0:8");
        MM_METHODS(preferences,
            {"setJavaScriptEnabled:", MM_IMP(+[](id s, SEL, std::uint64_t enabled) -> std::uint64_t {
                put(s, "javaScriptEnabled", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", enabled))); return 0;
            }), "v24@0:8B16"},
            {"javaScriptEnabled", MM_IMP(+[](id s, SEL) -> std::uint64_t {
                id value = get(s, "javaScriptEnabled"); return !value || send(value, "boolValue");
            }), "B16@0:8"});
        MM_METHODS(view,
            {"initWithFrame:configuration:", MM_IMP(view_init), "@56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"},
            {"init", MM_IMP(+[](id s, SEL) { return view_init(s, nullptr, nullptr, 0, 0, 640, 600); }), "@16@0:8"},
            {"configuration", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->configuration); }), "@16@0:8"},
            {"setNavigationDelegate:", MM_IMP(+[](id s, SEL, id d) -> std::uint64_t { as<webview>(s)->navigation_delegate = d; return 0; }), "v24@0:8@16"},
            {"navigationDelegate", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->navigation_delegate); }), "@16@0:8"},
            {"setUIDelegate:", MM_IMP(+[](id s, SEL, id d) -> std::uint64_t { as<webview>(s)->ui_delegate = d; return 0; }), "v24@0:8@16"},
            {"UIDelegate", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->ui_delegate); }), "@16@0:8"},
            {"loadRequest:", MM_IMP(load_request), "@24@0:8@16"},
            {"loadHTMLString:baseURL:", MM_IMP(load_html), "@32@0:8@16@24"},
            {"evaluateJavaScript:completionHandler:", MM_IMP(evaluate), "v32@0:8@16@?24"},
            {"stopLoading", MM_IMP(browser_action), "v16@0:8"},
            {"reload", MM_IMP(browser_action), "@16@0:8"},
            {"goBack", MM_IMP(browser_action), "@16@0:8"},
            {"goForward", MM_IMP(browser_action), "@16@0:8"},
            {"URL", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->url); }), "@16@0:8"},
            {"title", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->title); }), "@16@0:8"},
            {"isLoading", MM_IMP(+[](id s, SEL) -> std::uint64_t { return as<webview>(s)->loading; }), "B16@0:8"},
            {"setCustomUserAgent:", MM_IMP(+[](id s, SEL, id value) -> std::uint64_t {
                put(as<webview>(s)->configuration, "customUserAgent", value); return 0;
            }), "v24@0:8@16"},
            {"customUserAgent", MM_IMP(+[](id s, SEL) { return ret(get(as<webview>(s)->configuration, "customUserAgent")); }), "@16@0:8"},
            {"viewDidMoveToWindow", MM_IMP(+[](id s, SEL) -> std::uint64_t {
                auto *v = as<webview>(s);
                if (v->process && !v->view.window) v->process->send_json(mm_json(json_object{{"op", mm_json(std::string("hide"))}}));
                return 0;
            }), "v16@0:8"});
    }
    } // namespace ak

## Assistant

Объектная часть WebView и связь с браузерным процессом уже написаны: конфигурация, cookie store, загрузка страницы и сообщения JavaScript. Теперь собираю код и проверяю его на локальной HTML-странице с гостевыми обратными вызовами — это позволит проверить мост до повторного входа в Roblox.

## Activity

    file changes: Completed · 2 changes
    Update { move_path: None }: /mnt/media/machoman/modules/WebKit/framework.cpp
    @@ -1,2 +1,7 @@
     #include "synthesise.h"
    +#include "machoman/objc_host.h"
    +extern mm_objc_class webview_class __asm__("_OBJC_CLASS_$_WKWebView");
    +extern "C" __attribute__((visibility("default"))) mm_objc_class *machoman_webkit_view_class() {
    +    return &webview_class; // keep the AppKit dependency even with --as-needed
    +}
     extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    Update { move_path: None }: /mnt/media/machoman/src/dylibs/AppKit/webview.cpp
    @@ -8,2 +8,3 @@
     #include <chrono>
    +#include <cctype>
     #include <cmath>
    @@ -100,3 +101,3 @@
     }
    -Class plain(const char *name, Class *storage, Class *meta) {
    +Class plain(const char *name, mm_objc_class *storage, mm_objc_class *meta) {
         Class c = mm_objc_define_class(name, cls("NSObject"), sizeof(object), storage, meta);
    @@ -541,2 +542,5 @@
             {"init", MM_IMP(+[](id s, SEL) { return view_init(s, nullptr, nullptr, 0, 0, 640, 600); }), "@16@0:8"},
    +        {"initWithFrame:", MM_IMP(+[](id s, SEL, double x, double y, double w, double h) {
    +            return view_init(s, nullptr, nullptr, x, y, w, h);
    +        }), "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
             {"configuration", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->configuration); }), "@16@0:8"},

## Activity

    $ rg -n 'WebKit|stub.*framework|FRAMEWORKS|STUB_FRAMEWORK|add_subdirectory\(tests' CMakeLists.txt src/venv/venv.cpp | tail -n 35 && rg -n 'dispatch_get_main_queue|NSRunLoop|runUntilDate|NSDate' tests/guest_notifications.m tests/guest_http_cookies.m tests/guest_dispatch_data.c tests/CMakeLists.txt | head -n 40
    src/venv/venv.cpp:114:    for (auto dir : {MM_VENV_DIR_BIN, MM_VENV_DIR_ORIGINAL, MM_VENV_DIR_USR_LIB, MM_VENV_DIR_FRAMEWORKS})
    CMakeLists.txt:283:    add_subdirectory(modules/WebKit)
    CMakeLists.txt:381:    # (src/dylibs/stub-framework), installed under every framework machoman
    CMakeLists.txt:383:    add_library(stub_framework SHARED src/dylibs/symbol-synthesis/stub_framework.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
    CMakeLists.txt:384:    target_link_libraries(stub_framework PRIVATE machoman_core libobjc)
    CMakeLists.txt:385:    target_include_directories(stub_framework PRIVATE src/dylibs/symbol-synthesis)
    CMakeLists.txt:386:    target_compile_options(stub_framework PRIVATE -Wall -Wextra -fvisibility=hidden)
    CMakeLists.txt:387:    set_target_properties(stub_framework PROPERTIES
    CMakeLists.txt:389:        PREFIX "" OUTPUT_NAME "machoman-stub-framework" SUFFIX ".so"
    CMakeLists.txt:391:    set(MM_STUB_FRAMEWORKS
    CMakeLists.txt:393:        CoreGraphics:A Metal:A MetalKit:A GameController:A WebKit:A
    CMakeLists.txt:398:        list(REMOVE_ITEM MM_STUB_FRAMEWORKS Metal:A CoreGraphics:A)
    CMakeLists.txt:401:    foreach(entry IN LISTS MM_STUB_FRAMEWORKS)
    CMakeLists.txt:408:            # Ninja considers stub_framework up to date. Restore the stub
    CMakeLists.txt:411:            file(CREATE_LINK "${CMAKE_BINARY_DIR}/dylibs/usr/lib/machoman-stub-framework.so"
    CMakeLists.txt:416:            "${CMAKE_BINARY_DIR}/dylibs/usr/lib/machoman-stub-framework.so" "${dir}/${fw}.so")
    CMakeLists.txt:418:    add_custom_command(TARGET stub_framework POST_BUILD ${MM_STUB_COMMANDS}
    CMakeLists.txt:419:        COMMENT "Linking stub frameworks")
    CMakeLists.txt:424:        add_custom_command(TARGET stub_framework POST_BUILD
    CMakeLists.txt:426:                "machoman-stub-framework.so" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/${lib}.so")
    CMakeLists.txt:464:add_subdirectory(tests)
    tests/guest_dispatch_data.c:32:    dispatch_data_t b = dispatch_data_create(owned, 3, dispatch_get_main_queue(), ^{
    tests/guest_http_cookies.m:24:        check(session.expiresDate.timeIntervalSince1970 > NSDate.date.timeIntervalSince1970, "Max-Age expiry");
    ✓ • 0ms

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build --target AppKit WebKit -j 4
    [0/1] Re-running CMake...
    -- Performing Test HAVE_STDATOMIC
    -- Performing Test HAVE_STDATOMIC - Success
    -- Found OpenGL: /usr/lib/libOpenGL.so
    -- Configuring done (1.1s)
    -- Generating done (0.1s)
    -- Build files have been written to: /tmp/machoman-roblox-input-build
    [1/37] Automatic MOC for target machoman-webview
    [2/37] Building CXX object modules/WebKit/CMakeFiles/machoman-webview.dir/machoman-webview_autogen/mocs_compilation.cpp.o
    [3/37] Building CXX object modules/WebKit/CMakeFiles/machoman-webview.dir/browser.cpp.o
    /mnt/media/machoman/modules/WebKit/browser.cpp:95:5: warning: ignoring return value of function declared with 'nodiscard' attribute [-Wunused-result]
       95 |     channel_source.open(QIODevice::ReadOnly);
          |     ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    1 warning generated.
    [4/37] Linking CXX executable libexec/machoman-webview
    [5/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/appkit_main.cpp.o
    /mnt/media/machoman/src/dylibs/AppKit/appkit_main.cpp:17:6: warning: variable 'g_trace' set but not used [-Wunused-but-set-global]
       17 | bool g_trace = false;
          |      ^
    1 warning generated.
    [6/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/views.cpp.o
    /mnt/media/machoman/src/dylibs/AppKit/views.cpp:925:47: warning: unused parameter 'self' [-Wunused-parameter]
      925 | mm_two_doubles window_convertRectToBacking(id self, SEL, double x, double y, double width, double height) {
          |                                               ^
    /mnt/media/machoman/src/dylibs/AppKit/views.cpp:930:49: warning: unused parameter 'self' [-Wunused-parameter]
      930 | mm_two_doubles window_convertRectFromBacking(id self, SEL, double x, double y, double width, double height) {
          |                                                 ^
    /mnt/media/machoman/src/dylibs/AppKit/views.cpp:58:25: warning: variable 'g_panel' set but not used [-Wunused-but-set-global]
       58 | Class g_view, g_window, g_panel, g_view_controller, g_graphics_context;
          |                         ^
    3 warnings generated.
    [7/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/responder.cpp.o
    [8/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/controls.cpp.o
    [9/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/opengl.cpp.o
    [10/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/application.cpp.o
    /mnt/media/machoman/src/dylibs/AppKit/application.cpp:448:46: warning: unused parameter 'sender' [-Wunused-parameter]
      448 | std::uint64_t app_terminate(id self, SEL, id sender) {
          |                                              ^
    1 warning generated.
    [11/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/layers.cpp.o
    [12/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/text_view.cpp.o
    [13/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/stack_view.cpp.o
    [14/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/pasteboard.cpp.o
    [15/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/bitmap_drawing.cpp.o
    [16/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/webview.cpp.o
    [17/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/app_icon.cpp.o
    [18/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/icon_image.cpp.o
    [19/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/native_dialog.cpp.o
    [20/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/native_menu.cpp.o
    [21/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/file_panels.cpp.o
    [22/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/nib_archive.cpp.o
    [23/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/window_controller.cpp.o
    [24/37] Building C object CMakeFiles/AppKit.dir/generated/wayland/xdg-shell-protocol.c.o
    [25/37] Building C object CMakeFiles/AppKit.dir/generated/wayland/ext-data-control-v1-protocol.c.o
    [26/37] Building C object CMakeFiles/AppKit.dir/generated/wayland/xdg-foreign-v2-protocol.c.o
    [27/37] Building C object CMakeFiles/AppKit.dir/generated/wayland/xdg-toplevel-icon-v1-protocol.c.o
    [28/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/nib.cpp.o
    [29/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/clipboard_wayland.cpp.o
    [30/37] Building CXX object modules/WebKit/CMakeFiles/WebKit.dir/framework.cpp.o
    [31/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/symbol-synthesis/synthesise.cpp.o
    [32/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/platform_wayland.cpp.o
    [33/37] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/evilcocoa_adapter.cpp.o
    [34/37] Building CXX object modules/WebKit/CMakeFiles/WebKit.dir/__/__/src/dylibs/symbol-synthesis/synthesise.cpp.o
    [35/37] Linking CXX shared library dylibs/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit.so
    [36/37] Linking CXX shared library dylibs/System/Library/Frameworks/WebKit.framework/Versions/A/WebKit.so
    ✓ • 22.26s

## Activity

    $ sed -n '391,424p' CMakeLists.txt && sed -n '430,470p' CMakeLists.txt && sed -n '230,247p' tests/CMakeLists.txt
        set(MM_STUB_FRAMEWORKS
            AVFoundation:A CoreMedia:A VideoToolbox:A
            CoreGraphics:A Metal:A MetalKit:A GameController:A WebKit:A
            Security:A SystemConfiguration:A DiskArbitration:A IOKit:A
            UniformTypeIdentifiers:A ImageIO:A CoreML:A UserNotifications:A
            ServiceManagement:A CoreHaptics:A ForceFeedback:A IOSurface:A Contacts:A)
        if(TARGET MEVK_Metal)
            list(REMOVE_ITEM MM_STUB_FRAMEWORKS Metal:A CoreGraphics:A)
        endif()
        set(MM_STUB_COMMANDS)
        foreach(entry IN LISTS MM_STUB_FRAMEWORKS)
            string(REPLACE ":" ";" parts "${entry}")
            list(GET parts 0 fw)
            list(GET parts 1 ver)
            set(dir "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/${fw}.framework/Versions/${ver}")
            if(fw STREQUAL "Metal" OR fw STREQUAL "CoreGraphics")
                # A previous MEVK build can leave real framework binaries behind while
                # Ninja considers stub_framework up to date. Restore the stub
                # when configuring with MEVK disabled, even without a relink.
                file(MAKE_DIRECTORY "${dir}")
                file(CREATE_LINK "${CMAKE_BINARY_DIR}/dylibs/usr/lib/machoman-stub-framework.so"
                    "${dir}/${fw}.so" SYMBOLIC)
            endif()
            list(APPEND MM_STUB_COMMANDS COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}")
            list(APPEND MM_STUB_COMMANDS COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${CMAKE_BINARY_DIR}/dylibs/usr/lib/machoman-stub-framework.so" "${dir}/${fw}.so")
        endforeach()
        add_custom_command(TARGET stub_framework POST_BUILD ${MM_STUB_COMMANDS}
            COMMENT "Linking stub frameworks")
    
        # Plain /usr/lib dylibs a bundle links against that machoman does not
        # implement: the same stub object under their install names.
        foreach(lib IN ITEMS libffi.dylib libbsm.0.dylib)
            add_custom_command(TARGET stub_framework POST_BUILD
        add_library(libiconv SHARED src/dylibs/libiconv/libiconv.cpp)
        target_compile_options(libiconv PRIVATE -Wall -Wextra -fvisibility=hidden)
        set_target_properties(libiconv PROPERTIES
            PREFIX "" OUTPUT_NAME "libiconv.2.dylib" SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/usr/lib")
    
        # Guest runtimes: libc++/libc++abi/libunwind cross-built as arm64 Mach-O
        # dylibs (scripts/build-guest-runtimes.sh). They run as guest code, so the
        # C++ ABI never crosses into the host; only libSystem does.
        set(MM_GUEST_RT "${CMAKE_SOURCE_DIR}/third_party/guest-runtimes/install/lib")
        if(EXISTS "${MM_GUEST_RT}/libc++.1.dylib")
            add_custom_target(guest_runtimes ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MM_GUEST_RT}/libc++.1.dylib" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/libc++.1.dylib"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MM_GUEST_RT}/libc++abi.1.dylib" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/libc++abi.1.dylib"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MM_GUEST_RT}/libunwind.1.dylib" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/libunwind.1.dylib"
                COMMAND ${CMAKE_COMMAND} -E create_symlink "libc++abi.1.dylib" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/libc++abi.dylib"
                COMMAND ${CMAKE_COMMAND} -E create_symlink "libunwind.1.dylib" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/libunwind.dylib"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/usr/lib/system"
                COMMAND ${CMAKE_COMMAND} -E create_symlink "../libunwind.1.dylib" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/system/libunwind.dylib"
                COMMENT "Installing guest C++ runtimes into dylibs/usr/lib")
        else()
            message(WARNING "Guest C++ runtimes not built yet: run scripts/build-guest-runtimes.sh (needs a macOS SDK under third_party/sdk)")
        endif()
        set(MM_GUEST_X86_RT "${CMAKE_SOURCE_DIR}/third_party/guest-runtimes/x86_64/install/lib")
        if(EXISTS "${MM_GUEST_X86_RT}/libc++.1.dylib")
            add_custom_target(guest_runtimes_x86_64 ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/x86_64/usr/lib"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${MM_GUEST_X86_RT}" "${CMAKE_BINARY_DIR}/dylibs/x86_64/usr/lib"
                COMMENT "Installing native x86_64 guest C++ runtimes")
        endif()
    endif()
    
    # The tests come last: some of them link against the host dylibs above.
    add_subdirectory(tests)
                COMMAND ${MM_TEST_CLANG} --target=${MM_FOCUS_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework AppKit
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_pasteboard.m" -o "${MM_PASTEBOARD_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_pasteboard.m VERBATIM)
            add_custom_target(guest_pasteboard_${MM_FOCUS_ARCH}_binary ALL DEPENDS "${MM_PASTEBOARD_BINARY}")
            add_test(NAME guest_pasteboard_${MM_FOCUS_ARCH}
                COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
                    $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" pasteboard-${MM_FOCUS_ARCH})
            set_tests_properties(guest_pasteboard_${MM_FOCUS_ARCH} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "NSPasteboard typed data, named boards, writers and clipboard editing passed")
            set(MM_TEXT_BINARY "${MM_SMOKE_VENV}/bin/text-view-${MM_FOCUS_ARCH}")
            add_custom_command(OUTPUT "${MM_TEXT_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_FOCUS_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework AppKit
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_text_view.m" -o "${MM_TEXT_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_text_view.m VERBATIM)
            add_custom_target(guest_text_view_${MM_FOCUS_ARCH}_binary ALL DEPENDS "${MM_TEXT_BINARY}")
            add_test(NAME guest_text_view_${MM_FOCUS_ARCH}
    ✓ • 0ms

## Activity

    file changes: Completed · 2 changes
    Update { move_path: None }: /mnt/media/machoman/CMakeLists.txt
    @@ -399,2 +399,5 @@
         endif()
    +    if(TARGET WebKit)
    +        list(REMOVE_ITEM MM_STUB_FRAMEWORKS WebKit:A)
    +    endif()
         set(MM_STUB_COMMANDS)
    Add: /mnt/media/machoman/tests/guest_webview.m
    #import <AppKit/AppKit.h>
    #import <WebKit/WebKit.h>
    #include <stdio.h>
    #include <stdlib.h>
    
    static void check(BOOL ok, const char *message) {
        if (!ok) { fprintf(stderr, "webview: %s\n", message); exit(1); }
    }
    static unsigned cookieCallbacks, messageCallbacks, navigations, evaluations;
    @interface WebProbe : NSObject <WKNavigationDelegate, WKScriptMessageHandler>
    @end
    @implementation WebProbe
    - (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation {
        ++navigations;
        [view evaluateJavaScript:@"({answer: 6 * 7, text: 'Привет'})" completionHandler:^(id result, NSError *error) {
            check(error == nil && [result[@"answer"] intValue] == 42 && [result[@"text"] isEqual:@"Привет"],
                "JavaScript completion carries structured values across the guest ABI");
            ++evaluations;
        }];
    }
    - (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
        fprintf(stderr, "webview navigation: %s\n", error.localizedDescription.UTF8String); exit(1);
    }
    - (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
        check([message.name isEqual:@"probe"] && [message.body[@"answer"] intValue] == 42 && message.webView != nil,
            "window.webkit.messageHandlers posts a typed Cocoa message");
        ++messageCallbacks;
    }
    @end
    int main(int argc, char **argv) {
        @autoreleasepool {
            BOOL browser = argc > 1;
            NSApplication *app = NSApplication.sharedApplication;
            WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
            check(configuration.websiteDataStore == WKWebsiteDataStore.defaultDataStore &&
                configuration.userContentController != (id)configuration &&
                configuration.websiteDataStore.HTTPcookieStore != (id)configuration,
                "configuration owns distinct correctly typed child objects");
            WKWebsiteDataStore *temporary = WKWebsiteDataStore.nonPersistentDataStore;
            check(!temporary.persistent && configuration.websiteDataStore.persistent, "persistent and transient data stores");
            NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:@{NSHTTPCookieName:@"webview-test",
                NSHTTPCookieValue:@"value", NSHTTPCookieDomain:@"localhost", NSHTTPCookiePath:@"/"}];
            [temporary.httpCookieStore setCookie:cookie completionHandler:^{ ++cookieCallbacks; }];
            [temporary.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
                check(cookies.count == 1 && [cookies[0].value isEqual:@"value"], "cookie store asynchronous round trip");
                ++cookieCallbacks;
            }];
            configuration.websiteDataStore = temporary;
            WebProbe *probe = [WebProbe new];
            [configuration.userContentController addScriptMessageHandler:probe name:@"probe"];
            WKUserScript *script = [[WKUserScript alloc] initWithSource:@"window.webkit.messageHandlers.probe.postMessage({answer:42});"
                injectionTime:WKUserScriptInjectionTimeAtDocumentEnd forMainFrameOnly:YES];
            [configuration.userContentController addUserScript:script]; [script release];
            WKWebView *view = [[WKWebView alloc] initWithFrame:NSMakeRect(5, 10, 400, 300) configuration:configuration];
            view.navigationDelegate = probe;
            check([view isKindOfClass:NSView.class] && view.frame.size.width == 400 && view.configuration != configuration &&
                view.configuration.websiteDataStore == temporary, "NSView payload, Intel CGRect arguments and configuration copy");
            NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 500, 400)
                styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
            [window.contentView addSubview:view positioned:NSWindowAbove relativeTo:nil];
            check(view.window == window && view.superview == window.contentView, "WebView joins the real view tree without heap corruption");
            if (browser) [view loadHTMLString:@"<html><title>WebView test</title><body>local page</body></html>"
                baseURL:[NSURL URLWithString:@"http://localhost/"]];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15];
            while (cookieCallbacks < 2 || (browser && (!messageCallbacks || !evaluations))) {
                check(deadline.timeIntervalSinceNow > 0, "callbacks reach the main Cocoa queue");
                [app nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.05]
                    inMode:NSDefaultRunLoopMode dequeue:YES];
            }
            if (browser) check(navigations > 0 && [view.title isEqual:@"WebView test"], "navigation delegates and page title");
            view.navigationDelegate = nil;
            [view.configuration.userContentController removeScriptMessageHandlerForName:@"probe"];
            [view removeFromSuperview]; [view release]; [window release]; [configuration release]; [probe release];
        }
        puts("WKWebView hierarchy, configuration, cookies and guest callbacks passed");
        return 0;
    }

## User

можешь продолжить?

## Assistant

Продолжаю. Движок и Cocoa-часть уже собираются. Сейчас проверяю на обеих архитектурах cookies, загрузку HTML и сообщения JavaScript, затем снова запущу Roblox для проверки Sign In.

## Activity

    file changes: Completed · 2 changes
    Update { move_path: None }: /mnt/media/machoman/tests/CMakeLists.txt
    @@ -372,2 +372,23 @@
         set(MM_NATIVE_BINARY "${MM_SMOKE_VENV}/bin/native-smoke")
    +    foreach(MM_WEBVIEW_ARCH IN ITEMS arm64 x86_64)
    +        set(MM_WEBVIEW_BINARY "${MM_SMOKE_VENV}/bin/webview-${MM_WEBVIEW_ARCH}")
    +        add_custom_command(OUTPUT "${MM_WEBVIEW_BINARY}"
    +            COMMAND ${MM_TEST_CLANG} --target=${MM_WEBVIEW_ARCH}-apple-macos12
    +                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -fblocks -framework AppKit -framework WebKit
    +                "${CMAKE_CURRENT_SOURCE_DIR}/guest_webview.m" -o "${MM_WEBVIEW_BINARY}"
    +            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_webview.m VERBATIM)
    +        add_custom_target(guest_webview_${MM_WEBVIEW_ARCH}_binary ALL DEPENDS "${MM_WEBVIEW_BINARY}")
    +        add_test(NAME guest_webview_${MM_WEBVIEW_ARCH}
    +            COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
    +                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" webview-${MM_WEBVIEW_ARCH})
    +        set_tests_properties(guest_webview_${MM_WEBVIEW_ARCH} PROPERTIES TIMEOUT 20
    +            PASS_REGULAR_EXPRESSION "WKWebView hierarchy, configuration, cookies and guest callbacks passed")
    +        if(TARGET machoman-webview)
    +            add_test(NAME guest_webview_browser_${MM_WEBVIEW_ARCH}
    +                COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1 QT_QPA_PLATFORM=offscreen
    +                    $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" webview-${MM_WEBVIEW_ARCH} browser)
    +            set_tests_properties(guest_webview_browser_${MM_WEBVIEW_ARCH} PROPERTIES TIMEOUT 25
    +                PASS_REGULAR_EXPRESSION "WKWebView hierarchy, configuration, cookies and guest callbacks passed")
    +        endif()
    +    endforeach()
         foreach(MM_ERRNO_ARCH IN ITEMS arm64 x86_64)
    Update { move_path: None }: /mnt/media/machoman/tests/guest_webview.m
    @@ -36,3 +36,3 @@
                 configuration.userContentController != (id)configuration &&
    -            configuration.websiteDataStore.HTTPcookieStore != (id)configuration,
    +            configuration.websiteDataStore.httpCookieStore != (id)configuration,
                 "configuration owns distinct correctly typed child objects");

## Activity

    $ rg -n 'dispatch_drain|nextEventMatchingMask|app_nextEvent|runUntilDate' src/dylibs/AppKit/application.cpp src/dylibs/Foundation/misc.cpp | head -n 30 && sed -n '898,927p' src/runtime/loader.cpp
    src/dylibs/AppKit/application.cpp:458:std::uint64_t app_nextEventMatchingMask(id, SEL, std::uint64_t mask, id, id, std::uint64_t) {
    src/dylibs/AppKit/application.cpp:461:        if (!said) { said = true; std::fprintf(stderr, "AppKit: -[NSApplication nextEventMatchingMask:0x%llx]\n", static_cast<unsigned long long>(mask)); }
    src/dylibs/AppKit/application.cpp:984:        {"nextEventMatchingMask:untilDate:inMode:dequeue:", MM_IMP(app_nextEventMatchingMask), "@48@0:8Q16@24@32B40"},
    src/dylibs/Foundation/misc.cpp:1365:    if(l==g_main_loop)dispatch_drain_main_queue();
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
    ✓ • 0ms

## Activity

    $ sed -n '450,485p' src/dylibs/AppKit/application.cpp && rg -n 'runMode|runUntilDate|run_loop_run' src/dylibs/Foundation/misc.cpp && sed -n '1360,1408p' src/dylibs/Foundation/misc.cpp && sed -n '1,70p' tests/CMakeLists.txt
        if (responds(delegate, "applicationShouldTerminate:")) {
            const std::uint64_t reply = send(delegate, "applicationShouldTerminate:", ret(self));
            if (reply == 0 || reply == 2) return 0; /* NSTerminateCancel */
        }
        if (responds(delegate, "applicationWillTerminate:")) send(delegate, "applicationWillTerminate:", ret(self));
        app(self)->running = false;
        std::exit(0);
    }
    std::uint64_t app_nextEventMatchingMask(id, SEL, std::uint64_t mask, id, id, std::uint64_t) {
        if (trace()) {
            static bool said = false;
            if (!said) { said = true; std::fprintf(stderr, "AppKit: -[NSApplication nextEventMatchingMask:0x%llx]\n", static_cast<unsigned long long>(mask)); }
        }
        platform_event pe;
        if (!platform::next_event(pe, 0)) return 0;
        deliver(pe);
        return 0;
    }
    std::uint64_t app_sendEvent(id self, SEL, id event) {
        set_current_event(self, event);
        if (dragging_session_event(event)) return 0;
        id window = obj(send(event, "window"));
        if (file_panel_blocks_window(window)) return 0;
        if (dispatch_cocoa_popup_event(event)) return 0;
        if (send(event, "type") == ev_key_down && !text_input_has_priority(window, event) && app(self)->modals.empty() && app(self)->main_menu &&
            send(app(self)->main_menu, "performKeyEquivalent:", ret(event))) return 0;
        if (window) send(window, "sendEvent:", ret(event));
        return 0;
    }
    std::uint64_t app_postEvent(id, SEL, id, std::uint64_t) { return 0; }
    std::uint64_t app_noop(id, SEL, ...) { return 0; }
    std::uint64_t app_currentEvent(id self, SEL) { return ret(app(self)->current_event); }
    std::uint64_t app_modalWindow(id self, SEL) {
        const auto &sessions = app(self)->modals;
        return sessions.empty() ? 0 : ret(sessions.back()->window);
    }
    ✗ (1) • 0ms

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build --target AppKit WebKit guest_webview_arm64_binary guest_webview_x86_64_binary -j 4
    [0/1] Re-running CMake...
    -- Configuring done (0.6s)
    -- Generating done (0.1s)
    -- Build files have been written to: /tmp/machoman-roblox-input-build
    [1/50] Building CXX object CMakeFiles/machoman_core.dir/src/runtime/syscall_table.cpp.o
    [2/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/strings.cpp.o
    [3/50] Building CXX object CMakeFiles/libobjc.dir/src/dylibs/libobjc/objc.cpp.o
    [4/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/index_set.cpp.o
    [5/50] Building CXX object CMakeFiles/machoman_core.dir/src/runtime/loader.cpp.o
    /mnt/media/machoman/src/runtime/loader.cpp:2567:122: warning: missing field 'dependency' initializer [-Wmissing-designated-field-initializers]
     2567 |                                         .resolution = mm_import_resolution::missing, .lazy = bind.lazy, .weak = bind.weak};
          |                                                                                                                          ^
    1 warning generated.
    [6/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/collections.cpp.o
    [7/50] Linking CXX static library libmachoman_core.a
    [8/50] Linking CXX shared library dylibs/usr/lib/libobjc.A.dylib.so
    [9/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/plists.cpp.o
    [10/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/task.cpp.o
    [11/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/dispatch.cpp.o
    [12/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/dispatch_data.cpp.o
    [13/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/misc.cpp.o
    /mnt/media/machoman/src/dylibs/Foundation/misc.cpp:60:130: warning: variable 'g_charset' set but not used [-Wunused-but-set-global]
       60 |     g_apple_event_manager, g_notification_center, g_distributed_center, g_notification, g_exception, g_pool, g_timer, g_opqueue, g_charset;
          |                                                                                                                                  ^
    1 warning generated.
    [14/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/cookies.cpp.o
    [15/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/key_value.cpp.o
    [16/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/formatting.cpp.o
    [17/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/Foundation/threading.cpp.o
    [18/50] Building CXX object CMakeFiles/Foundation.dir/src/dylibs/symbol-synthesis/synthesise.cpp.o
    [19/50] Linking CXX shared library dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation.so
    [20/50] Building CXX object modules/MEVK/src/metal/CMakeFiles/MEVK_Metal.dir/metal.cpp.o
    [21/50] Building CXX object modules/MEVK/src/metal/CMakeFiles/MEVK_Metal.dir/render_metal.cpp.o
    [22/50] Linking CXX shared library dylibs/System/Library/Frameworks/Metal.framework/Versions/A/Metal.so
    [23/50] Building CXX object CMakeFiles/machoman.dir/src/cli/main.cpp.o
    [24/50] Linking CXX executable machoman
    [25/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/appkit_main.cpp.o
    /mnt/media/machoman/src/dylibs/AppKit/appkit_main.cpp:17:6: warning: variable 'g_trace' set but not used [-Wunused-but-set-global]
       17 | bool g_trace = false;
          |      ^
    1 warning generated.
    [26/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/views.cpp.o
    /mnt/media/machoman/src/dylibs/AppKit/views.cpp:925:47: warning: unused parameter 'self' [-Wunused-parameter]
      925 | mm_two_doubles window_convertRectToBacking(id self, SEL, double x, double y, double width, double height) {
          |                                               ^
    /mnt/media/machoman/src/dylibs/AppKit/views.cpp:930:49: warning: unused parameter 'self' [-Wunused-parameter]
      930 | mm_two_doubles window_convertRectFromBacking(id self, SEL, double x, double y, double width, double height) {
          |                                                 ^
    /mnt/media/machoman/src/dylibs/AppKit/views.cpp:58:25: warning: variable 'g_panel' set but not used [-Wunused-but-set-global]
       58 | Class g_view, g_window, g_panel, g_view_controller, g_graphics_context;
          |                         ^
    3 warnings generated.
    [27/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/responder.cpp.o
    [28/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/controls.cpp.o
    [29/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/opengl.cpp.o
    [30/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/application.cpp.o
    /mnt/media/machoman/src/dylibs/AppKit/application.cpp:448:46: warning: unused parameter 'sender' [-Wunused-parameter]
      448 | std::uint64_t app_terminate(id self, SEL, id sender) {
          |                                              ^
    1 warning generated.
    [31/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/layers.cpp.o
    [32/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/text_view.cpp.o
    [33/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/stack_view.cpp.o
    [34/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/pasteboard.cpp.o
    [35/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/webview.cpp.o
    [36/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/bitmap_drawing.cpp.o
    [37/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/app_icon.cpp.o
    [38/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/native_menu.cpp.o
    [39/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/window_controller.cpp.o
    [40/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/file_panels.cpp.o
    [41/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/nib.cpp.o
    [42/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/symbol-synthesis/synthesise.cpp.o
    [43/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/evilcocoa_adapter.cpp.o
    [44/50] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/platform_wayland.cpp.o
    [45/50] Building CXX object modules/WebKit/CMakeFiles/WebKit.dir/__/__/src/dylibs/symbol-synthesis/synthesise.cpp.o
    [46/50] Linking CXX shared library dylibs/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit.so
    [47/50] Linking CXX shared library dylibs/System/Library/Frameworks/WebKit.framework/Versions/A/WebKit.so
    [48/50] Generating guest-venv/bin/webview-arm64
    [49/50] Generating guest-venv/bin/webview-x86_64
    ✓ • 30.39s

## Activity

    $ sed -n '1358,1420p' src/dylibs/Foundation/misc.cpp && rg -n 'CFRunLoopRunInMode|dispatch_get_main_queue' src/dylibs/Foundation/misc.cpp src/dylibs/Foundation/dispatch.cpp && sed -n '92,115p' tests/CMakeLists.txt
            if(run){const auto argument=ret(request->argument);mm_objc_send(request->target,request->selector,1,&argument);}
            {std::lock_guard lock(request->mu);request->done=true;}
            request->cv.notify_all();
        }
    }
    void run_loop_drain(run_loop *l) {
        if (l == g_main_loop) url_cache_generation.fetch_add(1, std::memory_order_relaxed);
        if(l==g_main_loop)dispatch_drain_main_queue();
        run_loop_fire_selectors(l);
        run_loop_fire_timers(l);
        for (;;) {
            std::pair<void *, bool> item;
            {
                std::lock_guard lock(l->mu);
                if (l->blocks.empty()) return;
                item = l->blocks.front();
                l->blocks.erase(l->blocks.begin());
            }
            if (item.second) {
                std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> block(static_cast<id>(item.first), mm_objc_release);
                mm_objc_block_invoke(block.get(), 0, nullptr);
            }
            else if (api()->is_guest_code(reinterpret_cast<std::uintptr_t>(item.first))) api()->call_guest(reinterpret_cast<std::uintptr_t>(item.first), nullptr, 0);
            else reinterpret_cast<void (*)()>(item.first)();
        }
    }
    
    std::uint64_t runloop_run(id self, SEL) {
        run_loop *l = loop_of(self);
        for (;;) {
            run_loop_drain(l);
            std::unique_lock lock(l->mu);
            if (l->stopped) { l->stopped = false; return 0; }
            if (l->blocks.empty())
                l->cv.wait_for(lock, std::chrono::duration<double>(run_loop_timer_wait_locked(l, 0.1)));
        }
    }
    std::uint64_t runloop_stop(id self, SEL) { run_loop *l = loop_of(self); { std::lock_guard lock(l->mu); l->stopped = true; } l->cv.notify_all(); return 0; }
    std::uint64_t runloopc_currentRunLoop(id, SEL) { return ret(loop_object(loop_for_current_thread())); }
    std::uint64_t runloopc_mainRunLoop(id, SEL) { return ret(loop_object(g_main_loop ? g_main_loop : loop_for_current_thread())); }
    
    /* ---- NSAutoreleasePool / NSThread / misc classes --------------------------------- */
    
    struct pool_object { mm_objc_object base; void *token = nullptr; };
    std::uint64_t pool_init(id self, SEL) { as<pool_object>(self)->token = objc_autoreleasePoolPush_(); return ret(self); }
    std::uint64_t pool_drain(id self, SEL) { objc_autoreleasePoolPop_(as<pool_object>(self)->token); mm_objc_free_instance(self); return 0; }
    
    /* ---- NSAppleEventManager ------------------------------------------------------ */
    
    /* Apple events are how macOS delivers "open this document", "quit" and the
     * scripting commands to an application. Nothing sends them here, so the
     * manager keeps the handlers a program installs — which is all it asks of
     * it at startup — and there is no event to dispatch to them. */
    struct apple_event_manager { mm_objc_object base; std::vector<std::pair<id, SEL>> handlers; };
    
    std::uint64_t aem_shared(id c, SEL) {
        static id shared = nullptr;
        if (!shared) shared = &make<apple_event_manager>(reinterpret_cast<Class>(c))->base;
        return ret(shared);
    }
    std::uint64_t aem_setEventHandler(id self, SEL, id handler, SEL sel, std::uint32_t, std::uint32_t) {
        as<apple_event_manager>(self)->handlers.emplace_back(handler, sel);
        return 0;
    src/dylibs/Foundation/dispatch.cpp:168:/* dispatch_get_main_queue() is a macro over this symbol, so the guest
    src/dylibs/Foundation/dispatch.cpp:172:dispatch_queue *darwin_dispatch_get_main_queue() DARWIN(dispatch_get_main_queue);
    src/dylibs/Foundation/dispatch.cpp:173:dispatch_queue *darwin_dispatch_get_main_queue() { return &main_queue(); }
    src/dylibs/Foundation/misc.cpp:2227:std::int32_t CFRunLoopRunInMode_(id, double seconds, int) DARWIN(CFRunLoopRunInMode);
    src/dylibs/Foundation/misc.cpp:2228:std::int32_t CFRunLoopRunInMode_(id, double seconds, int) {
    add_test(NAME simd_helpers_no_avx2 COMMAND test_simd_helpers "${CMAKE_CURRENT_SOURCE_DIR}/simd_vectors.txt")
    set_tests_properties(simd_helpers_no_avx2 PROPERTIES ENVIRONMENT "MACHOMAN_DISABLE_AVX2=1")
    
    # CommonCrypto lives in the libSystem dylib, so the test links against it.
    if(TARGET libSystem)
        add_executable(test_commoncrypto test_commoncrypto.cpp)
        target_link_libraries(test_commoncrypto PRIVATE libSystem)
        add_test(NAME commoncrypto COMMAND test_commoncrypto)
        add_executable(test_uuid test_uuid.cpp)
        target_link_libraries(test_uuid PRIVATE libSystem)
        add_test(NAME uuid COMMAND test_uuid)
    endif()
    
    if(TARGET libobjc)
        add_executable(test_dynamic_stubs test_dynamic_stubs.cpp ../src/dylibs/symbol-synthesis/synthesise.cpp)
        target_include_directories(test_dynamic_stubs PRIVATE "${CMAKE_SOURCE_DIR}/include")
        target_link_libraries(test_dynamic_stubs PRIVATE libobjc ${CMAKE_DL_LIBS})
        add_test(NAME dynamic_stubs COMMAND test_dynamic_stubs)
        set_tests_properties(dynamic_stubs PROPERTIES TIMEOUT 10)
        add_executable(test_objc_concurrency test_objc_concurrency.cpp)
        target_include_directories(test_objc_concurrency PRIVATE "${CMAKE_SOURCE_DIR}/include")
        target_link_libraries(test_objc_concurrency PRIVATE libobjc)
        add_test(NAME objc_concurrency COMMAND test_objc_concurrency)
        set_tests_properties(objc_concurrency PROPERTIES TIMEOUT 10)
    ✓ • 0ms

## Activity

    file changes: Completed · 1 changes
    Update { move_path: None }: /mnt/media/machoman/tests/guest_webview.m
    @@ -32,3 +32,3 @@
             BOOL browser = argc > 1;
    -        NSApplication *app = NSApplication.sharedApplication;
    +        (void)NSApplication.sharedApplication;
             WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    @@ -66,4 +66,3 @@
                 check(deadline.timeIntervalSinceNow > 0, "callbacks reach the main Cocoa queue");
    -            [app nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.05]
    -                inMode:NSDefaultRunLoopMode dequeue:YES];
    +            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .05, true);
             }

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build --target guest_webview_arm64_binary guest_webview_x86_64_binary -j 4 && ctest --test-dir /tmp/machoman-roblox-input-build -R '^guest_webview_(arm64|x86_64)$' --output-on-failure
    [1/2] Generating guest-venv/bin/webview-arm64
    [2/2] Generating guest-venv/bin/webview-x86_64
    Test project /tmp/machoman-roblox-input-build
        Start 73: guest_webview_arm64
    1/2 Test #73: guest_webview_arm64 ..............   Passed    0.37 sec
        Start 75: guest_webview_x86_64
    2/2 Test #75: guest_webview_x86_64 .............   Passed    0.11 sec
    
    100% tests passed out of 2
    
    Total Test time (real) =   0.48 sec
    ✓ • 2.24s

## Activity

    $ git diff --stat -- src/runtime/loader.cpp src/dylibs/Foundation/misc.cpp CMakeLists.txt && sed -n '2225,2245p' src/dylibs/Foundation/misc.cpp && rg -n 'libexec|install\(' CMakeLists.txt src/venv/install.cpp | tail -n 25
     CMakeLists.txt                 |  69 +++-
     src/dylibs/Foundation/misc.cpp | 256 ++++++++++++--
     src/runtime/loader.cpp         | 782 +++++++++++++++++++++++++++++++++++------
     3 files changed, 959 insertions(+), 148 deletions(-)
    void CFRunLoopRun_() DARWIN(CFRunLoopRun);
    void CFRunLoopRun_() { runloop_run(loop_object(loop_for_current_thread()), nullptr); }
    std::int32_t CFRunLoopRunInMode_(id, double seconds, int) DARWIN(CFRunLoopRunInMode);
    std::int32_t CFRunLoopRunInMode_(id, double seconds, int) {
        /* Blocks sent to the main dispatch queue run here, on the thread that
         * owns the loop, as they do on macOS. */
        run_loop *l = loop_for_current_thread();
        run_loop_drain(l);
        if (seconds > 0) {
            std::unique_lock lock(l->mu);
            seconds = run_loop_timer_wait_locked(l, seconds);
            if (l->blocks.empty() && !l->stopped) l->cv.wait_for(lock, std::chrono::duration<double>(seconds));
        }
        run_loop_drain(l);
        std::lock_guard lock(l->mu);
        if (l->stopped) { l->stopped = false; return 2; } /* kCFRunLoopRunStopped */
        return 3;                                          /* kCFRunLoopRunTimedOut */
    }
    void CFRunLoopStop_(id loop) DARWIN(CFRunLoopStop);
    void CFRunLoopStop_(id loop) { runloop_stop(loop, nullptr); }
    void CFRunLoopWakeUp_(id loop) DARWIN(CFRunLoopWakeUp);
    ✓ • 0ms

## Activity

    $ ctest --test-dir /tmp/machoman-roblox-input-build -R '^guest_webview_browser_' --output-on-failure
    Test project /tmp/machoman-roblox-input-build
        Start 74: guest_webview_browser_arm64
    1/2 Test #74: guest_webview_browser_arm64 ......***Failed  Required regular expression not found. Regex=[WKWebView hierarchy, configuration, cookies and guest callbacks passed
    ] 15.50 sec
    dyld: /tmp/machoman-roblox-input-build/tests/guest-venv/bin/webview-arm64: missing dependency /System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork
    WebKit: browser helper started (PID 237683)
    webview: callbacks reach the main Cocoa queue
    
        Start 76: guest_webview_browser_x86_64
    2/2 Test #76: guest_webview_browser_x86_64 .....***Failed  Required regular expression not found. Regex=[WKWebView hierarchy, configuration, cookies and guest callbacks passed
    ] 15.18 sec
    dyld: /tmp/machoman-roblox-input-build/tests/guest-venv/bin/webview-x86_64: missing dependency /System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork
    WebKit: browser helper started (PID 238353)
    webview: callbacks reach the main Cocoa queue
    
    
    0% tests passed, 2 tests failed out of 2
    
    Total Test time (real) =  30.69 sec
    
    The following tests FAILED:
         74 - guest_webview_browser_arm64 (Failed)
         76 - guest_webview_browser_x86_64 (Failed)
    Errors while running CTest
    ✗ (8) • 30.55s

## Activity

    $ readelf -d /tmp/machoman-roblox-input-build/dylibs/System/Library/Frameworks/WebKit.framework/Versions/A/WebKit.so | rg 'NEEDED|RUNPATH' && ls -l /tmp/machoman-roblox-input-build/tests/guest-venv/System/Library/Frameworks/WebKit.framework/Versions/A
     0x0000000000000001 (NEEDED)             Shared library: [AppKit.so]
     0x0000000000000001 (NEEDED)             Shared library: [libobjc.A.dylib.so]
     0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]
     0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]
     0x0000000000000001 (NEEDED)             Shared library: [libgcc_s.so.1]
     0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
     0x000000000000001d (RUNPATH)            Library runpath: [/tmp/machoman-roblox-input-build/dylibs/System/Library/Frameworks/AppKit.framework/Versions/C:/tmp/machoman-roblox-input-build/dylibs/usr/lib:/tmp/machoman-roblox-input-build/dylibs/System/Library/Frameworks]
    ls: cannot access '/tmp/machoman-roblox-input-build/tests/guest-venv/System/Library/Frameworks/WebKit.framework/Versions/A': No such file or directory
    ✗ (2) • 0ms

## Activity

    $ pgrep -af '^(/tmp/machoman-roblox-input-build/libexec/machoman-webview|/usr/lib/qt6/QtWebEngineProcess)'
    ✗ (1) • 0ms

## Activity

    $ python3 -c 'import os,socket,subprocess,json,select,time; a,b=socket.socketpair(); p=subprocess.Popen(["/tmp/machoman-roblox-input-build/libexec/machoman-webview"],stdin=b,stdout=b,env=dict(os.environ,QT_QPA_PLATFORM="offscreen")); b.close(); a.sendall((json.dumps({"op":"configure","handlers":["probe"]})+"\n"+json.dumps({"op":"html","html":"<html><title>hello</title><body>test<script>window.webkit.messageHandlers.probe.postMessage({answer:42})</script></body></html>","base":"http://localhost/"})+"\n").encode()); end=time.monotonic()+8; print("helper PID",p.pid,flush=True); a.setblocking(False); last=b"";
        while time.monotonic()<end:
         if select.select([a],[],[],.5)[0]:
          data=a.recv(65536)
          if not data: break
          print(data.decode(),flush=True)
        p.terminate(); p.wait(timeout=5)'
    helper PID 240033
    {"event":"ready"}
    
    {"event":"started"}
    
    {"event":"url","url":"http://localhost/"}
    {"event":"title","title":"localhost"}
    
    {"event":"title","title":"hello"}
    
    {"event":"finished","ok":true,"url":"http://localhost/"}
    
    {"body":{"answer":42},"event":"message","name":"probe"}
    
    ✓ • 8.16s

## Activity

    file changes: Completed · 2 changes
    Update { move_path: None }: /mnt/media/machoman/src/dylibs/AppKit/webview.cpp
    @@ -289,2 +289,3 @@
         const std::string name = text(field(event, "event"));
    +    if (std::getenv("MACHOMAN_WEBKIT_TRACE")) std::fprintf(stderr, "WebKit: main queue event %s\n", name.c_str());
         if (name == "started") { v->loading = true; notify(v, self, "webView:didStartProvisionalNavigation:", v->navigation); }
    @@ -342,2 +343,3 @@
                     post(new event_task(shared_from_this(), std::move(event)));
    +            else if (std::getenv("MACHOMAN_WEBKIT_TRACE")) std::fprintf(stderr, "WebKit: malformed browser event\n");
                 pending.erase(0, end + 1);
    Update { move_path: None }: /mnt/media/machoman/tests/guest_webview.m
    @@ -65,2 +65,5 @@
             while (cookieCallbacks < 2 || (browser && (!messageCallbacks || !evaluations))) {
    +            if (deadline.timeIntervalSinceNow <= 0)
    +                fprintf(stderr, "webview callbacks: cookies=%u messages=%u navigation=%u evaluation=%u\n",
    +                    cookieCallbacks, messageCallbacks, navigations, evaluations);
                 check(deadline.timeIntervalSinceNow > 0, "callbacks reach the main Cocoa queue");

## Activity

    $ sed -n '1,95p' src/dylibs/libSystem/environment.cpp && sed -n '1,170p' src/runtime/environment.cpp && rg -n 'posix_spawn|envp|launch' src/dylibs/libSystem/processes.cpp | head -n 35
    #include "internal.h"
    #include <cerrno>
    #include <cstdlib>
    
    extern char **environ;
    extern "C" {
    /* glibc also exports a weak _environ alias. Protected visibility keeps our
     * internal references on this Darwin storage instead of preempting to libc. */
    __attribute__((visibility("protected"))) char **darwin_environ __asm__("_environ") = nullptr;
    
    char *darwin_getenv(const char *) DARWIN(getenv);
    char *darwin_getenv(const char *name) { return mm_host_getenv(mm_libsystem_api(), name); }
    
    int darwin_setenv(const char *, const char *, int) DARWIN(setenv);
    int darwin_setenv(const char *name, const char *value, int overwrite) {
        const auto *api=mm_libsystem_api();
        if (!api || api->version < 6) return ::setenv(name, value, overwrite);
        const int error=api->environment_set(name, value, overwrite);
        if (error) { errno=error; return -1; } return 0;
    }
    int darwin_unsetenv(const char *) DARWIN(unsetenv);
    int darwin_unsetenv(const char *name) {
        const auto *api=mm_libsystem_api();
        if (!api || api->version < 6) return ::unsetenv(name);
        const int error=api->environment_unset(name);
        if (error) { errno=error; return -1; } return 0;
    }
    int darwin_putenv(char *) DARWIN(putenv);
    int darwin_putenv(char *entry) {
        const auto *api=mm_libsystem_api();
        if (!api || api->version < 6) return ::putenv(entry);
        const int error=api->environment_put(entry);
        if (error) { errno=error; return -1; } return 0;
    }
    int darwin_clearenv() DARWIN(clearenv);
    int darwin_clearenv() {
        const auto *api=mm_libsystem_api();
        if (!api || api->version < 6) return ::clearenv();
        *api->environment()=nullptr; return 0;
    }
    char ***darwin_NSGetEnviron() DARWIN(_NSGetEnviron);
    char ***darwin_NSGetEnviron() {
        const auto *api=mm_libsystem_api();
        return api && api->version >= 6 ? api->environment() : &darwin_environ;
    }
    }
    void mm_libsystem_environment_init(const mm_host_api *api) {
        darwin_environ=api ? api->envp : environ;
        if (api && api->version >= 6) api->bind_environment(&darwin_environ);
    }
    #include "machoman/environment.h"
    #include <algorithm>
    #include <cerrno>
    #include <cstring>
    #include <new>
    
    namespace {
    bool matches(const char *entry, std::string_view name) {
        const std::string_view text(entry);
        return text.size() > name.size() && text.starts_with(name) && text[name.size()] == '=';
    }
    }
    bool mm_environment::valid_name(std::string_view name) {
        return !name.empty() && name.find_first_of("=\0", 0, 2) == std::string_view::npos;
    }
    char *mm_environment::own(std::string_view text) {
        auto storage = std::make_unique<char[]>(text.size() + 1);
        std::memcpy(storage.get(), text.data(), text.size());
        storage[text.size()] = '\0';
        char *result = storage.get();
        strings_.push_back(std::move(storage));
        return result;
    }
    void mm_environment::publish(std::span<char *const> entries) {
        auto array = std::make_unique<char *[]>(entries.size() + 1);
        std::copy(entries.begin(), entries.end(), array.get());
        array[entries.size()] = nullptr;
        auto *pointer = array.get();
        arrays_.push_back(std::move(array));
        *slot_ = pointer;
    }
    mm_environment::mm_environment(char *const *initial) {
        std::vector<char *> entries;
        for (auto p = initial; p && *p; ++p) entries.push_back(own(*p));
        publish(entries);
    }
    void mm_environment::bind(char ***storage) {
        std::lock_guard lock(mutex_);
        current_ = *slot_;
        slot_ = storage ? storage : &current_;
        *slot_ = current_;
    }
    char *mm_environment::get(const char *name) {
        if (!name || !valid_name(name)) return nullptr;
        std::lock_guard lock(mutex_);
        for (auto p = data(); p && *p; ++p)
            if (matches(*p, name)) return *p + std::strlen(name) + 1;
        return nullptr;
    }
    int mm_environment::set(const char *name, const char *value, int overwrite) {
        if (!name || !value || !valid_name(name)) return EINVAL;
        std::lock_guard lock(mutex_);
        try {
            std::vector<char *> entries;
            bool found = false;
            char *replacement = nullptr;
            for (auto p = data(); p && *p; ++p) {
                if (matches(*p, name)) {
                    if (!overwrite) return 0;
                    if (!found) {
                        replacement = own(std::string(name) + '=' + value);
                        entries.push_back(replacement);
                    }
                    found = true;
                } else entries.push_back(*p);
            }
            if (!found) entries.push_back(own(std::string(name) + '=' + value));
            publish(entries);
            return 0;
        } catch (const std::bad_alloc &) { return ENOMEM; }
    }
    int mm_environment::unset(const char *name) {
        if (!name || !valid_name(name)) return EINVAL;
        std::lock_guard lock(mutex_);
        try {
            std::vector<char *> entries;
            bool found = false;
            for (auto p = data(); p && *p; ++p) {
                if (matches(*p, name)) found = true;
                else entries.push_back(*p);
            }
            if (found) publish(entries);
            return 0;
        } catch (const std::bad_alloc &) { return ENOMEM; }
    }
    int mm_environment::put(char *entry) {
        if (!entry) return EINVAL;
        const std::string_view text(entry);
        const auto separator = text.find('=');
        if (separator == std::string_view::npos || !valid_name(text.substr(0, separator))) return EINVAL;
        std::lock_guard lock(mutex_);
        try {
            std::vector<char *> entries;
            bool found = false;
            for (auto p = data(); p && *p; ++p) {
                if (matches(*p, text.substr(0, separator))) {
                    if (!found) entries.push_back(entry);
                    found = true;
                } else entries.push_back(*p);
            }
            if (!found) entries.push_back(entry);
            publish(entries);
            return 0;
        } catch (const std::bad_alloc &) { return ENOMEM; }
    }
    std::vector<std::string> mm_environment::snapshot() {
        std::lock_guard lock(mutex_);
        std::vector<std::string> result;
        for (auto p = data(); p && *p; ++p) result.emplace_back(*p);
        return result;
    }
    49:struct attributes { posix_spawnattr_t native; short flags = 0; std::uint32_t mask = 0; };
    51:struct actions { posix_spawn_file_actions_t native; std::vector<file_action> records; };
    52:int apply(posix_spawn_file_actions_t &a, const file_action &record) {
    54:    case 0: return posix_spawn_file_actions_adddup2(&a, record.fd, record.target);
    55:    case 1: return posix_spawn_file_actions_addclose(&a, record.fd);
    56:    case 2: return posix_spawn_file_actions_addopen(&a, record.fd, record.path.c_str(), record.flags, record.mode);
    57:    case 3: return posix_spawn_file_actions_addchdir_np(&a, record.path.c_str());
    67:int close_by_default(posix_spawn_file_actions_t &a, const actions *source) {
    77:    if (keep.empty()) return posix_spawn_file_actions_addclosefrom_np(&a, 0);
    83:        const int error = posix_spawn_file_actions_adddup2(&a, keep[i], temporary + static_cast<int>(i));
    87:        const int error = posix_spawn_file_actions_addclose(&a, fd);
    90:    int error = posix_spawn_file_actions_addclosefrom_np(&a, temporary + static_cast<int>(keep.size()));
    93:        error = posix_spawn_file_actions_adddup2(&a, temporary + static_cast<int>(i), keep[i]);
    95:        error = posix_spawn_file_actions_addclose(&a, temporary + static_cast<int>(i));
    125:          char *const *argv, char *const *envp, bool search) {
    129:    const char *launcher = mm_host_launcher(api), *venv = mm_host_venv(api);
    131:    if (nested && (!launcher || !venv)) return 2;
    139:        args=mm_guest_launch_arguments(api, std::filesystem::absolute(program).string(), argv, mask);
    145:    for (auto p = envp; p && *p; ++p) environment.emplace_back(*p);
    151:    posix_spawn_file_actions_t filtered;
    154:        int error = posix_spawn_file_actions_init(&filtered);
    157:        if (error) { posix_spawn_file_actions_destroy(&filtered); return mm_darwin_errno(error); }
    162:    const int error = posix_spawn(&result, nested ? launcher : program.c_str(), filter ? &filtered : act ? &act->native : nullptr,
    164:    if (filter) posix_spawn_file_actions_destroy(&filtered);
    168:int exec(const char *path, char *const *argv, char *const *envp, bool search) {
    171:    if (!macho(program.c_str())) return execve(program.c_str(), argv, envp);
    173:    const char *launcher = mm_host_launcher(api), *venv = mm_host_venv(api);
    174:    if (!launcher || !venv) { errno = ENOENT; return -1; }
    177:    auto args=mm_guest_launch_arguments(api, std::filesystem::absolute(program).string(), argv, mask);
    181:    for (auto p = envp; p && *p; ++p) environment.emplace_back(*p);
    184:    return execve(launcher, raw.data(), env.data());
    201:int darwin_execve(const char *path, char *const *argv, char *const *envp) { return exec(path, argv, envp, false); }
    212:int darwin_posix_spawn(int *, const char *, const void *, const void *, char *const *, char *const *) DARWIN(posix_spawn);
    213:int darwin_posix_spawn(int *pid, const char *path, const void *actions, const void *attr, char *const *args, char *const *env) {
    216:int darwin_posix_spawnp(int *, const char *, const void *, const void *, char *const *, char *const *) DARWIN(posix_spawnp);
    ✓ • 0ms

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build --target AppKit guest_webview_arm64_binary guest_webview_x86_64_binary -j 4
    [1/4] Generating guest-venv/bin/webview-x86_64
    [2/4] Generating guest-venv/bin/webview-arm64
    [3/4] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/webview.cpp.o
    [4/4] Linking CXX shared library dylibs/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit.so
    ✓ • 4.07s

## Activity

    $ env MACHOMAN_WEBKIT_TRACE=1 ctest --test-dir /tmp/machoman-roblox-input-build -R '^guest_webview_browser_x86_64$' --output-on-failure
    Test project /tmp/machoman-roblox-input-build
        Start 76: guest_webview_browser_x86_64
    1/1 Test #76: guest_webview_browser_x86_64 .....***Failed  Required regular expression not found. Regex=[WKWebView hierarchy, configuration, cookies and guest callbacks passed
    ] 15.18 sec
    dyld: /tmp/machoman-roblox-input-build/tests/guest-venv/bin/webview-x86_64: missing dependency /System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork
    WebKit: browser helper started (PID 240911)
    WebKit: main queue event ready
    webview callbacks: cookies=2 messages=0 navigation=0 evaluation=0
    webview: callbacks reach the main Cocoa queue
    
    
    0% tests passed, 1 tests failed out of 1
    
    Total Test time (real) =  15.18 sec
    
    The following tests FAILED:
    Errors while running CTest
         76 - guest_webview_browser_x86_64 (Failed)
    ✗ (8) • 15.06s

## Activity

    $ pgrep -af '^(/tmp/machoman-roblox-input-build/libexec/machoman-webview|/tmp/machoman-roblox-input-build/machoman|/usr/lib/qt6/QtWebEngineProcess)'
    ✗ (1) • 0ms

## Activity

    file changes: Completed · 2 changes
    Update { move_path: None }: /mnt/media/machoman/modules/WebKit/browser.cpp
    @@ -156,2 +156,4 @@
                 const QString op = o["op"].toString();
    +            if (qEnvironmentVariableIsSet("MACHOMAN_WEBKIT_TRACE"))
    +                fprintf(stderr, "WebKit helper: command %s\n", op.toUtf8().constData());
                 if (op == "quit") { app.quit(); return; }
    Update { move_path: None }: /mnt/media/machoman/src/dylibs/AppKit/webview.cpp
    @@ -234,2 +234,4 @@
         bool send_json(const mm_json &command) {
    +        if (std::getenv("MACHOMAN_WEBKIT_TRACE"))
    +            std::fprintf(stderr, "WebKit: sending %s\n", text(field(command, "op")).c_str());
             const std::string line = mm_json_serialize(command) + '\n';

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build --target AppKit -j 4
    [1/6] Automatic MOC for target machoman-webview
    [2/5] Building CXX object modules/WebKit/CMakeFiles/machoman-webview.dir/browser.cpp.o
    /mnt/media/machoman/modules/WebKit/browser.cpp:95:5: warning: ignoring return value of function declared with 'nodiscard' attribute [-Wunused-result]
       95 |     channel_source.open(QIODevice::ReadOnly);
          |     ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    1 warning generated.
    [3/5] Linking CXX executable libexec/machoman-webview
    [4/5] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/webview.cpp.o
    [5/5] Linking CXX shared library dylibs/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit.so
    ✓ • 8.80s

## Assistant

Проверки дерева видов и cookies проходят на arm64 и Intel; краш из-за размера объекта устранён. Сам Chromium тоже загружает локальную страницу и отправляет JavaScript-сообщение. Сейчас исправляю связь между ним и Cocoa: в общем тесте до приложения доходит только событие запуска браузера.

## Activity

    $ pgrep -af '^/tmp/machoman-roblox-input-build/(libexec/machoman-webview|machoman)'
    245860 /tmp/machoman-roblox-input-build/machoman run /tmp/machoman-roblox-input-build/tests/guest-venv webview-x86_64 browser
    245861 /tmp/machoman-roblox-input-build/libexec/machoman-webview
    ✓ • 0ms

## Activity

    $ env MACHOMAN_WEBKIT_TRACE=1 ctest --test-dir /tmp/machoman-roblox-input-build -R '^guest_webview_browser_x86_64$' --output-on-failure
    Test project /tmp/machoman-roblox-input-build
        Start 76: guest_webview_browser_x86_64
    1/1 Test #76: guest_webview_browser_x86_64 .....***Failed  Required regular expression not found. Regex=[WKWebView hierarchy, configuration, cookies and guest callbacks passed
    ] 15.20 sec
    dyld: /tmp/machoman-roblox-input-build/tests/guest-venv/bin/webview-x86_64: missing dependency /System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork
    WebKit: sending configure
    WebKit: sending resize
    WebKit: browser helper started (PID 245861)
    WebKit: sending html
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit helper: command
    WebKit: main queue event ready
    webview callbacks: cookies=2 messages=0 navigation=0 evaluation=0
    webview: callbacks reach the main Cocoa queue
    
    
    0% tests passed, 1 tests failed out of 1
    
    Total Test time (real) =  15.21 sec
    
    The following tests FAILED:
         76 - guest_webview_browser_x86_64 (Failed)
    Errors while running CTest
    ✗ (8) • 15.08s

## Activity

    $ ps -eo pid,ppid,stat,pcpu,wchan:25,args
        PID    PPID STAT %CPU WCHAN                     COMMAND
          1       0 Ss    0.0 -                         /sbin/init
          2       0 S     0.0 -                         [kthreadd]
          3       2 S     0.0 -                         [pool_workqueue_release]
          4       2 I<    0.0 -                         [kworker/R-rcu_gp]
          5       2 I<    0.0 -                         [kworker/R-sync_wq]
          6       2 I<    0.0 -                         [kworker/R-kvfree_rcu_reclaim]
          7       2 I<    0.0 -                         [kworker/R-slub_flushwq]
          8       2 I<    0.0 -                         [kworker/R-netns]
         11       2 I<    0.0 -                         [kworker/0:0H-kblockd]
         13       2 I<    0.0 -                         [kworker/R-mm_percpu_wq]
         15       2 S     0.0 -                         [ksoftirqd/0]
         16       2 I     0.0 -                         [rcu_preempt]
         17       2 S     0.0 -                         [rcub/0]
         18       2 S     0.0 -                         [rcu_exp_par_gp_kthread_worker/0]
         19       2 S     0.0 -                         [rcu_exp_gp_kthread_worker]
         20       2 S     0.0 -                         [migration/0]
         21       2 S     0.0 -                         [kprobe-optimizer]
         22       2 S     0.0 -                         [idle_inject/0]
         23       2 S     0.0 -                         [cpuhp/0]
         24       2 S     0.0 -                         [cpuhp/1]
         25       2 S     0.0 -                         [idle_inject/1]
         26       2 S     0.0 -                         [migration/1]
         27       2 S     0.0 -                         [ksoftirqd/1]
         29       2 I<    0.0 -                         [kworker/1:0H-kblockd]
         30       2 S     0.0 -                         [cpuhp/2]
         31       2 S     0.0 -                         [idle_inject/2]
         32       2 S     0.0 -                         [migration/2]
         33       2 S     0.0 -                         [ksoftirqd/2]
         35       2 I<    0.0 -                         [kworker/2:0H-kblockd]
         36       2 S     0.0 -                         [cpuhp/3]
         37       2 S     0.0 -                         [idle_inject/3]
         38       2 S     0.0 -                         [migration/3]
         39       2 S     0.0 -                         [ksoftirqd/3]
         41       2 I<    0.0 -                         [kworker/3:0H-kblockd]
         42       2 S     0.0 -                         [cpuhp/4]
         43       2 S     0.0 -                         [idle_inject/4]
         44       2 S     0.0 -                         [migration/4]
         45       2 S     0.0 -                         [ksoftirqd/4]
         47       2 I<    0.0 -                         [kworker/4:0H-kblockd]
         48       2 S     0.0 -                         [cpuhp/5]
         49       2 S     0.0 -                         [idle_inject/5]
         50       2 S     0.0 -                         [migration/5]
         51       2 S     0.0 -                         [ksoftirqd/5]
         53       2 I<    0.0 -                         [kworker/5:0H-kblockd]
         54       2 S     0.0 -                         [kdevtmpfs]
         55       2 I<    0.0 -                         [kworker/R-inet_frag_wq]
         56       2 I     0.0 -                         [rcu_tasks_kthread]
         57       2 I     0.0 -                         [rcu_tasks_rude_kthread]
         58       2 S     0.0 -                         [kauditd]
         59       2 S     0.0 -                         [khungtaskd]
         60       2 S     0.0 -                         [oom_reaper]
         62       2 I<    0.0 -                         [kworker/R-writeback]
         63       2 S     0.1 -                         [kcompactd0]
         64       2 SN    0.0 -                         [ksmd]
         65       2 SN    0.0 -                         [khugepaged]
         66       2 I<    0.0 -                         [kworker/R-kblockd]
         67       2 I<    0.0 -                         [kworker/R-blkcg_punt_bio]
         68       2 I<    0.0 -                         [kworker/R-kintegrityd]
         69       2 S     0.0 -                         [irq/9-acpi]
         72       2 I<    0.0 -                         [kworker/R-tpm_dev_wq]
         73       2 I<    0.0 -                         [kworker/R-ata_sff]
         74       2 I<    0.0 -                         [kworker/R-edac-poller]
         75       2 I<    0.0 -                         [kworker/R-devfreq_wq]
         76       2 S     0.0 -                         [watchdogd]
         77       2 I<    0.0 -                         [kworker/R-quota_events_unbound]
         79       2 S     0.1 -                         [kswapd0]
         80       2 I<    0.0 -                         [kworker/R-kthrotld]
         81       2 S     0.0 -                         [irq/121-aerdrv]
         82       2 S     0.0 -                         [irq/122-aerdrv]
         83       2 S     0.0 -                         [irq/123-aerdrv]
         84       2 I<    0.0 -                         [kworker/R-acpi_thermal_pm]
         88       2 S     0.0 -                         [hwrng]
         89       2 S     0.0 -                         [scsi_eh_0]
         90       2 I<    0.0 -                         [kworker/R-scsi_tmf_0]
         91       2 S     0.0 -                         [scsi_eh_1]
         92       2 I<    0.0 -                         [kworker/R-scsi_tmf_1]
         93       2 S     0.0 -                         [scsi_eh_2]
         94       2 I<    0.0 -                         [kworker/R-scsi_tmf_2]
         95       2 S     0.0 -                         [scsi_eh_3]
         96       2 I<    0.0 -                         [kworker/R-scsi_tmf_3]
        101       2 I<    0.0 -                         [kworker/R-mld]
        102       2 I<    0.0 -                         [kworker/R-ipv6_addrconf]
        103       2 I<    0.0 -                         [kworker/R-kstrp]
        109       2 I<    0.0 -                         [kworker/0:1H-kblockd]
        110       2 I<    0.0 -                         [kworker/1:1H-kblockd]
        144       2 S     0.0 -                         [nv_queue]
        145       2 S     0.0 -                         [nv_mem_pool_scrubber_queue]
        146       2 S     0.0 -                         [nv_mem_pool_scrubber_queue]
        147       2 S     0.0 -                         [nv_mem_pool_scrubber_queue]
        148       2 S     0.0 -                         [nv_queue]
        150       2 S     0.0 -                         [nv_open_q]
        151       2 S     0.0 -                         [irq/132-nvidia]
        153       2 S     0.0 -                         [nv_queue]
        156       2 S     0.3 -                         [nvidia-modeset/kthread_q]
        157       2 S     0.0 -                         [nvidia-modeset/deferred_close_kthread_q]
        158       2 S     0.0 -                         [UVM global queue]
        159       2 S     0.0 -                         [UVM deferred release queue]
        160       2 S     0.0 -                         [UVM Tools Event Queue]
        196       2 S     0.0 -                         [scsi_eh_4]
        197       2 I<    0.0 -                         [kworker/R-scsi_tmf_4]
        198       2 S     0.0 -                         [usb-storage]
        199       2 I<    0.0 -                         [kworker/R-uas]
        261       2 I<    0.0 -                         [kworker/2:1H-kblockd]
        262       2 I<    0.0 -                         [kworker/R-btrfs-worker]
        263       2 I<    0.0 -                         [kworker/R-btrfs-delalloc]
        264       2 I<    0.0 -                         [kworker/R-btrfs-flush_delalloc]
        265       2 I<    0.0 -                         [kworker/R-btrfs-cache]
        266       2 I<    0.0 -                         [kworker/R-btrfs-fixup]
        267       2 I<    0.0 -                         [kworker/R-btrfs-endio]
        268       2 I<    0.0 -                         [kworker/R-btrfs-endio-meta]
        269       2 I<    0.0 -                         [kworker/R-btrfs-rmw]
        270       2 I<    0.0 -                         [kworker/R-btrfs-endio-write]
        271       2 I<    0.0 -                         [kworker/R-btrfs-freespace-write]
        272       2 I<    0.0 -                         [kworker/R-btrfs-delayed-meta]
        273       2 I<    0.0 -                         [kworker/R-btrfs-qgroup-rescan]
        274       2 S     0.0 -                         [btrfs-cleaner]
        275       2 S     0.0 -                         [btrfs-transaction]
        284       2 I<    0.0 -                         [kworker/5:1H-kblockd]
        286       2 I<    0.0 -                         [kworker/4:1H-kblockd]
        288       2 I<    0.0 -                         [kworker/3:1H-kblockd]
        327       1 Ss    0.0 -                         /usr/lib/systemd/systemd-journald
        340       1 Ss    0.0 -                         /usr/lib/systemd/systemd-userdbd
        358       2 S     0.0 -                         [psimon]
        367       1 Ssl   0.0 -                         /usr/lib/systemd/systemd-timesyncd
        375       1 Ss    0.0 -                         /usr/lib/systemd/systemd-udevd
        379       2 S     0.0 -                         [psimon]
        452       2 I<    0.0 -                         [kworker/R-cfg80211]
        456       2 S     0.0 -                         [irq/135-iwlwifi:default_queue]
        457       2 S     0.0 -                         [irq/136-iwlwifi:queue_1]
        458       2 S     0.0 -                         [irq/137-iwlwifi:queue_2]
        460       2 S     0.0 -                         [irq/138-iwlwifi:queue_3]
        461       2 S     0.0 -                         [irq/139-iwlwifi:queue_4]
        462       2 S     0.0 -                         [irq/140-iwlwifi:queue_5]
        463       2 S     0.0 -                         [irq/141-iwlwifi:queue_6]
        464       2 S     0.0 -                         [irq/142-iwlwifi:exception]
        517       1 Ss    0.0 -                         /usr/bin/dbus-broker-launch --scope system --audit
        519     517 S     0.0 -                         dbus-broker --log 10 --controller 9 --machine-id b28600a38d4c4d8281d34edd6de0b9c2 --max-bytes 536870912 --max-fds 4096 --max-matches 131072 --audit
        521       1 Ssl   0.0 -                         /usr/bin/NetworkManager --no-daemon
        522       1 Ss    0.0 -                         /usr/lib/bluetooth/bluetoothd
        524       1 Ss    0.0 -                         /usr/lib/systemd/systemd-logind
        567       1 Ss    0.0 -                         /usr/bin/wpa_supplicant -u -s -O /run/wpa_supplicant
        576       1 SLs   0.0 -                         greetd
        580       2 S     0.0 -                         [psimon]
        620     576 SLs+  0.0 -                         /usr/bin/greetd --session-worker 12
        649       1 Ss    0.0 -                         bash /home/monsler/zapret-discord-youtube-linux/service.sh daemon
        690       1 Ss    0.0 -                         /usr/lib/systemd/systemd --user
        692     690 S     0.0 -                         (sd-pam)
        699     690 Ss    0.0 skb_wait_for_more_packets /usr/bin/ydotoold
        700     620 Sl+   0.0 poll_schedule_timeout.con /usr/bin/start-hyprland
        803     700 Sl+  10.4 do_epoll_wait             Hyprland --watchdog-fd 4
        836       1 Ss    0.0 -                         /home/monsler/zapret-discord-youtube-linux/nfqws --daemon --dpi-desync-fwmark=0x40000000 --qnum=220 --filter-udp=443 --hostlist=lists/list-general.txt --hostlist=lists/list-general-user.txt --hostlist-exclude=lists/list-exclude.txt --hostlist-exclude=lists/list-exclude-user.txt --ipset-exclude=lists/ipset-exclude.txt --ipset-exclude=lists/ipset-exclude-user.txt --dpi-desync=fake --dpi-desync-repeats=11 --dpi-desync-fake-quic=bin/quic_initial_www_google_com.bin --new --filter-udp=19294-19344,50000-50100 --filter-l7=discord,stun --dpi-desync=fake --dpi-desync-fake-discord=bin/quic_initial_dbankcloud_ru.bin --dpi-desync-fake-stun=bin/quic_initial_dbankcloud_ru.bin --dpi-desync-repeats=6 --new --filter-tcp=2053,2083,2087,2096,8443 --hostlist-domains=discord.media --dpi-desync=fake fakedsplit --dpi-desync-split-pos=1 --dpi-desync-fooling=badseq --dpi-desync-badseq-increment=2 --dpi-desync-repeats=8 --dpi-desync-fake-tls-mod=rnd,dupsid,sni=www.google.com --new --filter-tcp=443 --hostlist=lists/list-google.txt --ip-id=zero --dpi-desync=fake fakedsplit --dpi-desync-split-pos=1 --dpi-desync-fooling=badseq --dpi-desync-badseq-increment=2 --dpi-desync-repeats=8 --dpi-desync-fake-tls-mod=rnd,dupsid,sni=www.google.com --new --filter-tcp=80,443 --hostlist=lists/list-general.txt --hostlist=lists/list-general-user.txt --hostlist-exclude=lists/list-exclude.txt --hostlist-exclude=lists/list-exclude-user.txt --ipset-exclude=lists/ipset-exclude.txt --ipset-exclude=lists/ipset-exclude-user.txt --dpi-desync=fake fakedsplit --dpi-desync-split-pos=1 --dpi-desync-fooling=badseq --dpi-desync-badseq-increment=2 --dpi-desync-repeats=8 --dpi-desync-fake-tls-mod=rnd,dupsid,sni=www.google.com --dpi-desync-fake-http=bin/tls_clienthello_max_ru.bin --new --filter-udp=443 --ipset=lists/ipset-all.txt --hostlist-exclude=lists/list-exclude.txt --hostlist-exclude=lists/list-exclude-user.txt --ipset-exclude=lists/ipset-exclude.txt --ipset-exclude=lists/ipset-exclude-user.txt --dpi-desync=fake --dpi-desync-repeats=11 --dpi-desync-fake-quic=bin/quic_initial_www_google_com.bin --new --filter-tcp=80,443,8443 --ipset=lists/ipset-all.txt --hostlist-exclude=lists/list-exclude.txt --hostlist-exclude=lists/list-exclude-user.txt --ipset-exclude=lists/ipset-exclude.txt --ipset-exclude=lists/ipset-exclude-user.txt --dpi-desync=fake fakedsplit --dpi-desync-split-pos=1 --dpi-desync-fooling=badseq --dpi-desync-badseq-increment=2 --dpi-desync-repeats=8 --dpi-desync-fake-tls-mod=rnd,dupsid,sni=www.google.com --dpi-desync-fake-http=bin/tls_clienthello_max_ru.bin --new --filter-tcp=1024-65535 --ipset=lists/ipset-all.txt --ipset-exclude=lists/ipset-exclude.txt --ipset-exclude=lists/ipset-exclude-user.txt --dpi-desync=fake fakedsplit --dpi-desync-any-protocol=1 --dpi-desync-cutoff=n3 --dpi-desync-split-pos=1 --dpi-desync-fooling=badseq --dpi-desync-badseq-increment=2 --dpi-desync-repeats=8 --dpi-desync-fake-tls-mod=rnd,dupsid,sni=www.google.com --dpi-desync-fake-http=bin/tls_clienthello_max_ru.bin --new --filter-udp=1024-65535 --ipset=lists/ipset-all.txt --ipset-exclude=lists/ipset-exclude.txt --ipset-exclude=lists/ipset-exclude-user.txt --dpi-desync=fake --dpi-desync-repeats=10 --dpi-desync-any-protocol=1 --dpi-desync-fake-unknown-udp=bin/quic_initial_dbankcloud_ru.bin --dpi-desync-cutoff=n2
        838     649 S     0.0 -                         sleep infinity
        852       2 S     0.0 -                         [nvidia-drm/timeline-7]
        859     690 Ss    0.0 do_epoll_wait             /usr/bin/dbus-broker-launch --scope user
        860     859 S     0.0 do_epoll_wait             dbus-broker --log 11 --controller 10 --machine-id b28600a38d4c4d8281d34edd6de0b9c2 --max-bytes 100000000000000 --max-fds 25000000000000 --max-matches 5000000000
        862     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/dconf-service
        869     803 Sl+   2.0 poll_schedule_timeout.con qs -c ii
        872     803 Sl+   0.0 futex_do_wait             hypridle
        877     803 S+    0.0 poll_schedule_timeout.con wl-paste --type text --watch bash -c cliphist store && qs -c $qsConfig ipc call cliphistService update
        879     803 S+    0.0 poll_schedule_timeout.con wl-paste --type image --watch bash -c cliphist store && qs -c $qsConfig ipc call cliphistService update
        881       1 Sl+   0.0 poll_schedule_timeout.con /usr/lib/geoclue-2.0/demos/agent
        888     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/xdg-desktop-portal
        896     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/xdg-permission-store
        903     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/xdg-document-portal
        904     803 S+    0.0 do_epoll_wait             Xwayland :0 -rootless -core -listenfd 71 -listenfd 72 -displayfd 111 -wm 108
        910     903 Ss    0.0 -                         fusermount3 -o rw,nosuid,nodev,fsname=portal,auto_unmount,subtype=portal -- /run/user/1000/doc
        913     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/xdg-desktop-portal-gtk
        924       1 Ssl   0.0 -                         /usr/lib/polkit-1/polkitd --no-debug --log-level=notice
        934       2 S     0.0 -                         [nvidia-drm/timeline-8]
        935     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/at-spi-bus-launcher
        941     935 S     0.0 do_epoll_wait             /usr/bin/dbus-broker-launch --config-file=/usr/share/defaults/at-spi2/accessibility.conf --scope user
        942     941 S     0.0 do_epoll_wait             dbus-broker --log 10 --controller 9 --machine-id b28600a38d4c4d8281d34edd6de0b9c2 --max-bytes 100000000000000 --max-fds 6400000 --max-matches 5000000000
        959     690 Ssl   0.0 poll_schedule_timeout.con /usr/lib/at-spi2-registryd --use-gnome-session
        965     690 Ssl   0.0 do_epoll_wait             /usr/bin/pipewire
        966     690 Ssl   0.4 poll_schedule_timeout.con /usr/bin/wireplumber
        986       2 S<    0.0 -                         [krfcommd]
       1013       1 Sl    0.0 futex_do_wait             hyprsunset
       1054     869 Sl+   0.0 poll_schedule_timeout.con /usr/bin/nmcli monitor
       1111       2 S     0.0 -                         [nvidia-drm/timeline-9]
       1126       1 Ssl   0.0 -                         /usr/lib/upowerd
       1144       2 S     0.0 -                         [nvidia-drm/timeline-c]
       1274       1 Sl    0.6 -                         /usr/bin/hiddify
       1368       2 S     0.0 -                         [nvidia-drm/timeline-2b]
       1376       2 S     0.0 -                         [nvidia-drm/timeline-2c]
       1377       2 S     0.0 -                         [nvidia-drm/timeline-2d]
       1442       2 S     0.0 -                         [nvidia-drm/timeline-2f]
       1771       1 Ss    3.9 -                         /usr/bin/mount.ntfs-3g /dev/sda1 /mnt/media -o rw
       8299       2 S     0.0 -                         [irq/133-mei_me]
       8312       2 S     0.0 -                         [nvidia]
       8948       2 S     0.0 -                         [nvidia-drm/timeline-b3]
      20849       1 S+    0.0 do_wait                   bash /home/monsler/.config/hypr/hyprland/scripts/launch_first_available.sh google-chrome-stable zen-browser firefox brave chromium microsoft-edge-stable opera librewolf
      20850   20849 Sl+  12.0 poll_schedule_timeout.con /usr/lib/firefox/firefox
      20868       1 Sl    0.0 poll_schedule_timeout.con /usr/lib/firefox/crashhelper 20850 9 /tmp/ 11 20261110030740
      21077       2 S     0.0 -                         [nvidia-drm/timeline-fa]
      21079   20850 S+    0.0 unix_stream_read_generic  /usr/lib/firefox/firefox -contentproc -ipcHandle 0 -signalPipe 1 -initialChannelId {235edab7-fe00-471f-bd34-62513bfa13a5} -parentPid 20850 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 1 forkserver
      21096   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -parentBuildID 20261110030740 -prefsHandle 0:41150 -prefMapHandle 1:304356 -sandboxReporter 2 -chrootClient 3 -ipcHandle 4 -initialChannelId {ca113d4d-613a-42bb-bf7c-4fa85392fdd8} -parentPid 20850 -crashHelperPid 20868 -crashHelper 5 -crashReporter 6 -appDir /usr/lib/firefox/browser 2 socket
      21106   21079 Sl+   0.8 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:41294 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {af7fe740-9130-49b7-9ae5-1aa1c823a46d} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 3 tab
      21115   21079 Sl+   0.8 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -parentBuildID 20261110030740 -prefsHandle 0:41294 -prefMapHandle 1:304356 -sandboxReporter 2 -chrootClient 3 -ipcHandle 4 -initialChannelId {a261d295-ab0f-4d11-99f7-e3015b7952c5} -parentPid 20850 -crashHelperPid 20868 -crashHelper 5 -crashReporter 6 -appDir /usr/lib/firefox/browser 4 rdd
      21156   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:41381 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {458d411f-22d2-410c-9b83-107d1251cee5} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 5 tab
      21406   21079 Sl+   0.2 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -parentBuildID 20261110030740 -sandboxingKind 0 -prefsHandle 0:59533 -prefMapHandle 1:304356 -sandboxReporter 2 -chrootClient 3 -ipcHandle 4 -initialChannelId {402bfa5d-136d-4c14-be27-3bba58383649} -parentPid 20850 -crashHelperPid 20868 -crashHelper 5 -crashReporter 6 -appDir /usr/lib/firefox/browser 6 utility
      21417   21079 Sl+   3.5 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:52546 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {92d43ed6-1bf2-44a1-9fab-ac02e9bb2ac6} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 8 tab
      21643   21079 Sl+   7.9 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53114 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {17c219ff-0602-44de-97a9-01a3cb718b96} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 11 tab
      21724       2 S     0.0 -                         [nvidia-drm/timeline-109]
      25490       1 Sl    0.3 poll_schedule_timeout.con /home/monsler/.config/discord/app-1.0.168/Discord --url --
      25516   25490 S     0.0 poll_schedule_timeout.con /home/monsler/.config/discord/app-1.0.168/Discord --type=zygote --no-zygote-sandbox
      25517   25490 S     0.0 do_wait                   /home/monsler/.config/discord/app-1.0.168/Discord --type=zygote
      25519   25517 S     0.0 poll_schedule_timeout.con /home/monsler/.config/discord/app-1.0.168/Discord --type=zygote
      25534       1 Sl    0.0 do_epoll_wait             /home/monsler/.config/discord/app-1.0.168/chrome_crashpad_handler --monitor-self-annotation=ptype=crashpad-handler --no-rate-limit --database=/home/monsler/.config/discord/Crashpad --url=https://f.a.k/e --annotation=_productName=discord --annotation=_version=1.0.168 --annotation=lsb-release=Arch Linux --annotation=plat=Linux --annotation=prod=Electron --annotation=ver=44.10.0 --initial-client-fd=38 --shared-client-connection
      25562   25516 Sl    0.9 futex_do_wait             /home/monsler/.config/discord/app-1.0.168/Discord --type=gpu-process --ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=25534 --enable-crash-reporter=954005c1-6dc2-4c3c-8e1d-e3520dbb1a4a,no_channel --user-data-dir=/home/monsler/.config/discord --gpu-preferences=YAAAAAAAAAAgAQAEAAAAAAAAAAAAAGAASAAAAAAAAAABAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAACAAAAAAAAAAAAAAAAAAAAMAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAACAAAAAAAAAAMAAAAAQAAAAAAAAAAAAAACAAAAAAAAAAIAAAAAAAAAA== --shared-files --field-trial-handle=3,i,3958781733018370569,3778886553142342217,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,PdfUseShowSaveFilePicker --disable-features=AllowAggressiveThrottlingWithWebSocket,DropInputEventsWhilePaintHolding,HardwareMediaKeyHandling,IntensiveWakeUpThrottling,LocalNetworkAccessChecks,MediaSessionService,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation,UseEcoQoSForBackgroundProcess,WaylandWpColorManagerV1,WinRetrieveSuggestionsOnlyOnDemand --variations-seed-version --pseudonymization-salt-handle=7,i,853047477570082328,4448811282575538907,4 --trace-process-track-uuid=3190708988185955192
      25568   25490 Sl    0.0 futex_do_wait             /home/monsler/.config/discord/app-1.0.168/Discord --type=utility --utility-sub-type=network.mojom.NetworkService --lang=ru --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=25534 --enable-crash-reporter=954005c1-6dc2-4c3c-8e1d-e3520dbb1a4a,no_channel --user-data-dir=/home/monsler/.config/discord --standard-schemes=disclip --secure-schemes=disclip,sentry-ipc --bypasscsp-schemes=sentry-ipc --cors-schemes=disclip,sentry-ipc --fetch-schemes=disclip,sentry-ipc --streaming-schemes=disclip --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,3958781733018370569,3778886553142342217,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,PdfUseShowSaveFilePicker --disable-features=AllowAggressiveThrottlingWithWebSocket,DropInputEventsWhilePaintHolding,HardwareMediaKeyHandling,IntensiveWakeUpThrottling,LocalNetworkAccessChecks,MediaSessionService,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation,UseEcoQoSForBackgroundProcess,WaylandWpColorManagerV1,WinRetrieveSuggestionsOnlyOnDemand --variations-seed-version --pseudonymization-salt-handle=7,i,853047477570082328,4448811282575538907,4 --trace-process-track-uuid=3190708989122997041
      25621       2 S     0.0 -                         [nvidia-drm/timeline-111]
      25649   25562 S     0.0 skb_wait_for_more_packets /home/monsler/.config/discord/app-1.0.168/Discord --type=broker
      25665       2 S     0.0 -                         [nvidia-drm/timeline-112]
      25671       2 S     0.0 -                         [nvidia-drm/timeline-113]
      25674       2 S     0.0 -                         [nvidia-drm/timeline-114]
      25732   25490 Sl    2.6 futex_do_wait             /home/monsler/.config/discord/app-1.0.168/Discord --type=renderer --crashpad-handler-pid=25534 --enable-crash-reporter=954005c1-6dc2-4c3c-8e1d-e3520dbb1a4a,no_channel --user-data-dir=/home/monsler/.config/discord --standard-schemes=disclip --secure-schemes=disclip,sentry-ipc --bypasscsp-schemes=sentry-ipc --cors-schemes=disclip,sentry-ipc --fetch-schemes=disclip,sentry-ipc --streaming-schemes=disclip --app-path=/home/monsler/.config/discord/app-1.0.168/resources/app.asar --no-sandbox --no-zygote --enable-blink-features=EnumerateDevices,AudioOutputDevices --autoplay-policy=no-user-gesture-required --ozone-platform=wayland --lang=ru --num-raster-threads=3 --enable-main-frame-before-activation --renderer-client-id=7 --launch-time-ticks=2080376481 --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,3958781733018370569,3778886553142342217,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,PdfUseShowSaveFilePicker --disable-features=AllowAggressiveThrottlingWithWebSocket,DropInputEventsWhilePaintHolding,HardwareMediaKeyHandling,IntensiveWakeUpThrottling,LocalNetworkAccessChecks,MediaSessionService,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation,UseEcoQoSForBackgroundProcess,WaylandWpColorManagerV1,WinRetrieveSuggestionsOnlyOnDemand --variations-seed-version --pseudonymization-salt-handle=7,i,853047477570082328,4448811282575538907,4 --trace-process-track-uuid=3190708992871164437 --enable-node-leakage-in-renderers
      25855     690 SLsl  0.4 do_epoll_wait             /usr/bin/pipewire-pulse
      25905   25490 Sl    0.0 futex_do_wait             /home/monsler/.config/discord/app-1.0.168/Discord --type=utility --utility-sub-type=audio.mojom.AudioService --lang=ru --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=25534 --enable-crash-reporter=954005c1-6dc2-4c3c-8e1d-e3520dbb1a4a,no_channel --user-data-dir=/home/monsler/.config/discord --standard-schemes=disclip --secure-schemes=disclip,sentry-ipc --bypasscsp-schemes=sentry-ipc --cors-schemes=disclip,sentry-ipc --fetch-schemes=disclip,sentry-ipc --streaming-schemes=disclip --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,3958781733018370569,3778886553142342217,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,PdfUseShowSaveFilePicker --disable-features=AllowAggressiveThrottlingWithWebSocket,DropInputEventsWhilePaintHolding,HardwareMediaKeyHandling,IntensiveWakeUpThrottling,LocalNetworkAccessChecks,MediaSessionService,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation,UseEcoQoSForBackgroundProcess,WaylandWpColorManagerV1,WinRetrieveSuggestionsOnlyOnDemand --variations-seed-version --pseudonymization-salt-handle=7,i,853047477570082328,4448811282575538907,4 --trace-process-track-uuid=3190708993808206286
      28230   21079 Sl+   0.7 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53214 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {be8bfd27-de2c-4a4d-8495-04da1f230667} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 13 tab
      35022   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53214 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {1f118128-a95a-4743-9cae-8269324154f3} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 15 tab
      46302   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53280 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {1122d683-0dbf-47e5-9a72-d907e627c07e} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 16 tab
      94508   20850 S+    0.0 poll_schedule_timeout.con bwrap --unshare-all --die-with-parent --chdir / --ro-bind /usr /usr --dev /dev --ro-bind-try /etc/ld.so.cache /etc/ld.so.cache --ro-bind-try /nix/store /nix/store --tmpfs /tmp-home --tmpfs /tmp-run --clearenv --setenv HOME /tmp-home --setenv XDG_RUNTIME_DIR /tmp-run --setenv XDG_RUNTIME_DIR /run/user/1000 --symlink /usr/lib /lib --symlink /usr/lib /lib64 --seccomp 299 /usr/lib/glycin-loaders/2+/glycin-image-rs --dbus-fd 298
      94510   94508 S+    0.0 do_wait                   bwrap --unshare-all --die-with-parent --chdir / --ro-bind /usr /usr --dev /dev --ro-bind-try /etc/ld.so.cache /etc/ld.so.cache --ro-bind-try /nix/store /nix/store --tmpfs /tmp-home --tmpfs /tmp-run --clearenv --setenv HOME /tmp-home --setenv XDG_RUNTIME_DIR /tmp-run --setenv XDG_RUNTIME_DIR /run/user/1000 --symlink /usr/lib /lib --symlink /usr/lib /lib64 --seccomp 299 /usr/lib/glycin-loaders/2+/glycin-image-rs --dbus-fd 298
      94511   94510 Sl+   0.0 futex_do_wait             /usr/lib/glycin-loaders/2+/glycin-image-rs --dbus-fd 298
     122545       2 S     0.0 -                         [nvidia-drm/timeline-38c]
     134805       1 Ssl   3.3 poll_schedule_timeout.con /usr/share/code/code --ozone-platform-hint=wayland --gtk-version=4 --ignore-gpu-blocklist --enable-features=TouchpadOverscrollHistoryNavigation --enable-wayland-ime --password-store=gnome-libsecret
     134808  134805 S     0.0 poll_schedule_timeout.con /usr/share/code/code --type=zygote --no-zygote-sandbox
     134809  134805 S     0.0 do_wait                   /usr/share/code/code --type=zygote
     134811  134809 S     0.0 poll_schedule_timeout.con /usr/share/code/code --type=zygote
     134831       1 Sl    0.0 do_epoll_wait             /usr/share/code/chrome_crashpad_handler --monitor-self-annotation=ptype=crashpad-handler --no-rate-limit --database=/home/monsler/.config/Code/Crashpad --url=appcenter://code?aid=fba07a4d-84bd-4fc8-a125-9640fc8ce171&uid=f2f72b41-0b7d-4ef6-a71d-580af1a1dfd0&iid=f2f72b41-0b7d-4ef6-a71d-580af1a1dfd0&sid=f2f72b41-0b7d-4ef6-a71d-580af1a1dfd0 --annotation=_companyName=Microsoft --annotation=_productName=VSCode --annotation=_version=1.145.2 --annotation=lsb-release=Arch Linux --annotation=plat=Linux --annotation=prod=Electron --annotation=ver=43.8.3 --initial-client-fd=39 --shared-client-connection
     134850  134808 Sl    9.0 futex_do_wait             /usr/share/code/code --type=gpu-process --ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --gpu-preferences=WAAAAAAAAAAgAQAMAAAAAAAAAAAAAGAAQAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAACAAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAgAAAAAAAAACAAAAAAAAAAIAAAAAAAAAA== --shared-files --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708988185955192
     134853  134805 Sl    0.0 futex_do_wait             /usr/share/code/code --type=utility --utility-sub-type=network.mojom.NetworkService --lang=en-US --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708989122997041
     134886       2 S     0.0 -                         [nvidia-drm/timeline-433]
     134894  134811 Sl   20.1 futex_do_wait             /usr/share/code/code --type=renderer --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --app-path=/usr/share/code/resources/app --enable-sandbox --enable-blink-features=HighlightAPI --max-active-webgl-contexts=32 --ozone-platform=wayland --disable-blink-features=FontMatchingCTMigration,StandardizedBrowserZoom, --lang=en-US --num-raster-threads=3 --enable-main-frame-before-activation --renderer-client-id=4 --time-ticks-at-unix-epoch=-1794520513840335 --launch-time-ticks=7723377847 --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,251396
     134909  134850 S     0.0 skb_wait_for_more_packets /usr/share/code/code --type=broker
     134916       2 S     0.0 -                         [nvidia-drm/timeline-434]
     134919       2 S     0.0 -                         [nvidia-drm/timeline-435]
     134922       2 S     0.0 -                         [nvidia-drm/timeline-436]
     134944  134805 Sl    0.0 futex_do_wait             /usr/share/code/code --type=utility --utility-sub-type=node.mojom.NodeService --lang=en-US --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708990997080739
     134974  134805 Sl    2.7 futex_do_wait             /usr/share/code/code --type=utility --utility-sub-type=node.mojom.NodeService --lang=en-US --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708991934122588
     134994  134805 Sl    0.7 futex_do_wait             /usr/share/code/code --type=utility --utility-sub-type=node.mojom.NodeService --lang=en-US --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --dns-result-order=ipv4first --experimental-network-inspection --inspect-port=0 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708992871164437
     135014  134805 Sl    0.0 futex_do_wait             /usr/share/code/code --type=utility --utility-sub-type=node.mojom.NodeService --lang=en-US --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708993808206286
     135015  134805 Sl    0.4 futex_do_wait             /usr/share/code/code --type=utility --utility-sub-type=node.mojom.NodeService --lang=en-US --service-sandbox-type=none --render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=134831 --enable-crash-reporter=646a13e7-2a69-4313-b166-34483b8849df,no_channel --user-data-dir=/home/monsler/.config/Code --standard-schemes=vscode-webview,vscode-file --enable-sandbox --secure-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --cors-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --fetch-schemes=vscode-webview,vscode-file,vscode-remote-resource,vscode-managed-remote-resource --service-worker-schemes=vscode-webview --code-cache-schemes=vscode-webview,vscode-file --shared-files=v8_context_snapshot_data:100 --field-trial-handle=3,i,4672657102295035646,18019222827208060507,262144 --enable-features=DocumentPolicyIncludeJSCallStacksInCrashReports,EarlyEstablishGpuChannel,EstablishGpuChannelAsync,GlobalShortcutsPortal,GlobalShortcutsPortalPreferredTrigger,PdfUseShowSaveFilePicker,TouchpadOverscrollHistoryNavigation,UpdateCompositorMemoryPolicyOnScreenChange --disable-features=CalculateNativeWinOcclusion,DropInputEventsWhilePaintHolding,LocalNetworkAccessChecks,ScreenAIOCREnabled,SpareRendererForSitePerProcess,TraceSiteInstanceGetProcessCreation --variations-seed-version --pseudonymization-salt-handle=7,i,11743457633127618444,2513964917018995927,4 --trace-process-track-uuid=3190708994745248135
     135054  134944 Sl    0.0 futex_do_wait             /usr/share/code/resources/app/node_modules.asar.unpacked/@github/copilot-sdk-linux-x64/prebuilds/linux-x64/copilot-runtime --headless --no-auto-update --log-level info --stdio --no-auto-login
     135084  134805 Sl    0.0 poll_schedule_timeout.con dconf watch /system/proxy/
     135111  134974 Ss    0.0 futex_do_wait             /usr/bin/fish --init-command source "/usr/share/code/resources/app/out/vs/workbench/contrib/terminal/common/scripts/shellIntegration.fish"
     135134  134974 Ss+   0.0 poll_schedule_timeout.con /usr/bin/fish --init-command source "/usr/share/code/resources/app/out/vs/workbench/contrib/terminal/common/scripts/shellIntegration.fish"
     135400  135111 Sl+   6.6 futex_do_wait             /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex -c model_provider=agentrouter
     135755  135400 Sl    0.0 do_epoll_wait             /usr/lib/node_modules/@openai/codex/node_modules/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex-code-mode-host
     142021  134994 Sl    0.0 do_epoll_wait             /usr/share/code/code /usr/share/code/resources/app/extensions/json-language-features/server/dist/node/jsonServerMain --node-ipc --clientProcessId=134994
     146395  135400 Z+    0.0 -                         [firefox] <defunct>
     156155     690 Ssl   0.0 poll_schedule_timeout.con /usr/bin/dolphin --daemon
     156166       1 Ssl   0.0 -                         /usr/lib/udisks2/udisksd
     172572       1 Sl    3.6 poll_schedule_timeout.con /usr/bin/Telegram --
     172630       2 S     0.0 -                         [nvidia-drm/timeline-5cc]
     176478  135400 Z+    0.0 -                         [firefox] <defunct>
     178474       1 Ss    0.0 -                         /usr/bin/dirmngr --homedir /etc/pacman.d/gnupg --supervised
     202836       2 I     0.0 -                         [kworker/3:1-mm_percpu_wq]
     208981       2 I     0.0 -                         [kworker/u24:3-btrfs-endio]
     220145   21079 Sl+   1.1 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53281 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {b0d6bad8-da5d-415d-a2e1-69eb11cdfb06} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 36 tab
     220935       2 I     0.0 -                         [kworker/2:2-events]
     221281       2 I<    0.0 -                         [kworker/u25:2-rb_allocator]
     222277       2 I     0.0 -                         [kworker/u24:15-btrfs-endio]
     222279       2 I     0.0 -                         [kworker/u24:19-btrfs-endio]
     222280       2 I     0.0 -                         [kworker/u24:20-btrfs-endio-meta]
     224272       2 I     0.0 -                         [kworker/4:0-mm_percpu_wq]
     224783       2 I     0.0 -                         [kworker/1:2-events]
     225462       2 S     0.0 -                         [nvidia-drm/timeline-9f5]
     225652       2 I<    0.0 -                         [kworker/u25:1-rb_allocator]
     226477       2 S     0.0 -                         [nvidia-drm/timeline-a08]
     226479       2 S     0.0 -                         [nvidia-drm/timeline-a0d]
     226481       2 S     0.0 -                         [nvidia-drm/timeline-a0f]
     226483       2 S     0.0 -                         [nvidia-drm/timeline-a11]
     226485       2 S     0.0 -                         [nvidia-drm/timeline-a13]
     227103       2 I     0.0 -                         [kworker/0:2-events]
     227105       1 Sl    0.0 poll_schedule_timeout.con /usr/bin/dolphin
     227254     690 Ssl   0.0 poll_schedule_timeout.con /usr/bin/kded6
     227273     690 Ssl   0.1 poll_schedule_timeout.con /usr/bin/swappy -f /home/monsler/Изображения/photo_2026-11-13_04-43-40.jpg
     227594     690 Ssl   0.1 poll_schedule_timeout.con /usr/bin/swappy -f /home/monsler/Изображения/photo_2026-11-13_05-04-00.jpg
     227944     690 Ssl   0.1 poll_schedule_timeout.con /usr/bin/swappy -f /home/monsler/Изображения/photo_2026-11-13_05-04-14.jpg
     232957       2 I     0.0 -                         [kworker/0:1-events]
     233206       2 I     0.0 -                         [kworker/3:0-cgroup_release]
     233526       2 I     0.0 -                         [kworker/2:1-events]
     234930       2 I     0.0 -                         [kworker/4:2-events]
     235886       2 I     0.0 -                         [kworker/5:1-mm_percpu_wq]
     239159       2 I     0.0 -                         [kworker/u24:0-btrfs-endio]
     239164       2 I     0.0 -                         [kworker/u24:8-btrfs-endio]
     239171       2 I     0.0 -                         [kworker/u24:14-btrfs-endio-write]
     239295       2 I     0.0 -                         [kworker/u24:18-events_unbound]
     241544       2 I     0.0 -                         [kworker/1:1-memcg]
     241553       2 I     0.0 -                         [kworker/5:0-cgroup_free]
     241658       2 I     0.0 -                         [kworker/0:0-events]
     243641   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53281 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {e45cc25d-9b1a-4e11-b59b-153276ee4b6b} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 61 tab
     243995       2 I     0.0 -                         [kworker/4:1-cgroup_free]
     244191   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53281 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {e7488fcb-03c3-4410-9d80-837ae55ac0b1} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 62 tab
     244296       2 I     0.0 -                         [kworker/u24:1-btrfs-endio]
     244297       2 I     0.0 -                         [kworker/u24:2-btrfs-endio]
     244298       2 I     0.0 -                         [kworker/u24:4-btrfs-endio]
     244299       2 I     0.0 -                         [kworker/u24:5-events_unbound]
     244438     340 S     0.0 -                         systemd-userwork: waiting...
     244599       2 I     0.0 -                         [kworker/2:0-events]
     244670       2 I     0.0 -                         [kworker/3:2-events]
     245358     340 S     0.0 -                         systemd-userwork: waiting...
     245749   21079 Sl+   0.0 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53281 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {29ca68c3-5548-48b9-b032-de8b3f7688f8} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 63 tab
     246201     340 S     0.0 -                         systemd-userwork: waiting...
     246431   21079 Sl+   0.2 poll_schedule_timeout.con /usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:53281 -prefMapHandle 1:304356 -jsInitHandle 2:161360 -parentBuildID 20261110030740 -sandboxReporter 3 -chrootClient 4 -ipcHandle 5 -initialChannelId {97385583-caf4-47cc-9846-fb84b2b72464} -parentPid 20850 -crashHelperPid 20868 -crashHelper 6 -crashReporter 7 -greomni /usr/lib/firefox/omni.ja -appomni /usr/lib/firefox/browser/omni.ja -appDir /usr/lib/firefox/browser 64 tab
     246592  135400 Rs    133 -                         ps -eo pid,ppid,stat,pcpu,wchan:25,args
    ✓ • 0ms

## Activity

    $ rg -n 'serialize|indent|\\n' src/common/json.cpp | tail -n 30 && tail -n 75 src/common/json.cpp
    12:    void space() { while (at < input.size() && (input[at]==' ' || input[at]=='\t' || input[at]=='\n' || input[at]=='\r')) ++at; }
    49:                case 'n': out += '\n'; break;
    141:std::string serialize(const mm_json &value, unsigned depth) {
    147:    const std::string indent((depth+1)*2, ' '), end_indent(depth*2, ' ');
    150:        for (const auto &[key, entry] : *v) { if (out.size()>1) out+=','; out+='\n'+indent+quote(key)+": "+serialize(entry, depth+1); }
    151:        if (!v->empty()) out+='\n'+end_indent;
    156:    for (const auto &entry : v) { if (out.size()>1) out+=','; out+='\n'+indent+serialize(entry, depth+1); }
    157:    if (!v.empty()) out+='\n'+end_indent;
    168:std::string mm_json_serialize(const mm_json &value) { return serialize(value, 0)+'\n'; }
                    expect(',');
                } while (true);
                return mm_json(std::move(out));
            }
            if (take('[')) {
                mm_json::array out;
                if (!take(']')) do {
                    out.push_back(parse(depth+1));
                    if (take(']')) return mm_json(std::move(out));
                    expect(',');
                } while (true);
                return mm_json(std::move(out));
            }
            for (const auto literal : {std::string_view("null"), std::string_view("true"), std::string_view("false")})
                if (input.substr(at, literal.size())==literal) {
                    at += literal.size();
                    if (literal=="null") return {};
                    return mm_json(literal=="true");
                }
            const auto start=at;
            if (input[at]=='-') ++at;
            if (at == input.size()) fail("truncated number");
            if (input[at]=='0') ++at;
            else { if (input[at]<'1' || input[at]>'9') fail("invalid number"); while (at<input.size() && input[at]>='0' && input[at]<='9') ++at; }
            auto digits=[&] {
                const auto first=at;
                while (at<input.size() && input[at]>='0' && input[at]<='9') ++at;
                if (at==first) fail("missing number digits");
            };
            if (at<input.size() && input[at]=='.') { ++at; digits(); }
            if (at<input.size() && (input[at]=='e' || input[at]=='E')) {
                ++at; if (at<input.size() && (input[at]=='+' || input[at]=='-')) ++at; digits();
            }
            return mm_json(mm_json_number{std::string(input.substr(start, at-start))});
        }
    };
    std::string quote(std::string_view text) {
        constexpr char hex[]="0123456789abcdef";
        std::string out="\"";
        for (char raw : text) {
            const auto c=static_cast<unsigned char>(raw);
            if (c=='"' || c=='\\') { out+='\\'; out+=raw; }
            else if (c<32) { out+="\\u00"; out+=hex[c >> 4]; out+=hex[c & 15]; }
            else out+=raw;
        }
        return out+'"';
    }
    std::string serialize(const mm_json &value, unsigned depth) {
        if (std::holds_alternative<std::nullptr_t>(value.value)) return "null";
        if (const auto *v=std::get_if<bool>(&value.value)) return *v ? "true" : "false";
        if (const auto *v=std::get_if<mm_json_number>(&value.value)) return v->text;
        if (const auto *v=std::get_if<std::string>(&value.value)) return quote(*v);
        std::string out;
        const std::string indent((depth+1)*2, ' '), end_indent(depth*2, ' ');
        if (const auto *v=std::get_if<mm_json::object>(&value.value)) {
            out="{";
            for (const auto &[key, entry] : *v) { if (out.size()>1) out+=','; out+='\n'+indent+quote(key)+": "+serialize(entry, depth+1); }
            if (!v->empty()) out+='\n'+end_indent;
            return out+'}';
        }
        out="[";
        const auto &v=std::get<mm_json::array>(value.value);
        for (const auto &entry : v) { if (out.size()>1) out+=','; out+='\n'+indent+serialize(entry, depth+1); }
        if (!v.empty()) out+='\n'+end_indent;
        return out+']';
    }
    }
    bool mm_json_parse(std::string_view source, mm_json &out, std::string &error) {
        try {
            parser p{source}; auto value=p.parse(); p.space();
            if (p.at != source.size()) p.fail("trailing JSON data");
            out=std::move(value); error.clear(); return true;
        } catch (const std::runtime_error &e) { error=e.what(); return false; }
    }
    std::string mm_json_serialize(const mm_json &value) { return serialize(value, 0)+'\n'; }
    ✓ • 0ms

## User

можешь продолжить?

## Assistant

Продолжаю. Нашёл причину сбоя связи: сериализатор выдавал JSON с переносами строк, а браузер ожидал одну команду на строку. Исправляю формат передачи и повторяю проверку загрузки страницы и JavaScript.

## Activity

    file changes: Completed · 2 changes
    Update { move_path: None }: /mnt/media/machoman/modules/WebKit/browser.cpp
    @@ -94,3 +94,6 @@
         QFile channel_source(":/qtwebchannel/qwebchannel.js");
    -    channel_source.open(QIODevice::ReadOnly);
    +    if (!channel_source.open(QIODevice::ReadOnly)) {
    +        fprintf(stderr, "WebKit: JavaScript channel resource is unavailable\n");
    +        return 1;
    +    }
         const QString channel_js = QString::fromUtf8(channel_source.readAll());
    Update { move_path: None }: /mnt/media/machoman/src/dylibs/AppKit/webview.cpp
    @@ -236,3 +236,7 @@
                 std::fprintf(stderr, "WebKit: sending %s\n", text(field(command, "op")).c_str());
    -        const std::string line = mm_json_serialize(command) + '\n';
    +        // The manifest serializer pretty-prints JSON and escapes all control
    +        // characters in strings. Strip its formatting newlines for this wire
    +        // protocol, which uses one complete JSON object per line.
    +        std::string line = mm_json_serialize(command);
    +        std::erase(line, '\n'); line += '\n';
             std::lock_guard lock(write_mutex);

## Activity

    $ pgrep -af '^(/tmp/machoman-roblox-input-build/(libexec/machoman-webview|machoman))'
    ✗ (1) • 0ms

## Activity

    $ cmake --build /tmp/machoman-roblox-input-build --target AppKit WebKit -j 4
    [0/1] Re-running CMake...
    -- Configuring done (0.7s)
    -- Generating done (0.1s)
    -- Build files have been written to: /tmp/machoman-roblox-input-build
    [1/8] Automatic MOC for target machoman-webview
    [2/7] Building CXX object modules/WebKit/CMakeFiles/machoman-webview.dir/browser.cpp.o
    [3/7] Linking CXX executable libexec/machoman-webview
    [4/7] Building CXX object CMakeFiles/AppKit.dir/src/dylibs/AppKit/webview.cpp.o
    [5/7] Linking CXX shared library dylibs/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit.so
    [6/7] Linking CXX shared library dylibs/System/Library/Frameworks/WebKit.framework/Versions/A/WebKit.so
    ✓ • 9.49s

## Activity

    $ sed -n '1,170p' src/dylibs/AppKit/webview.cpp && rg -n 'URLRequest|loadRequest|WKWeb|webView:' work/roblox-import-audit.tsv tests/guest_webview.m | head -n 20
    /* WKWebView is a real NSView. Chromium lives in an isolated host helper;
     * only the main Cocoa queue sends messages back into guest delegates. */
    #include "appkit.h"
    #include "machoman/json.h"
    #include <algorithm>
    #include <atomic>
    #include <cerrno>
    #include <chrono>
    #include <cctype>
    #include <cmath>
    #include <dlfcn.h>
    #include <filesystem>
    #include <memory>
    #include <mutex>
    #include <spawn.h>
    #include <sys/socket.h>
    #include <sys/wait.h>
    #include <thread>
    #include <unistd.h>
    
    extern char **environ;
    MM_FW_CLASS(WKWebView); MM_FW_CLASS(WKWebViewConfiguration);
    MM_FW_CLASS(WKWebsiteDataStore); MM_FW_CLASS(WKHTTPCookieStore);
    MM_FW_CLASS(WKUserContentController); MM_FW_CLASS(WKUserScript);
    MM_FW_CLASS(WKScriptMessage); MM_FW_CLASS(WKNavigation); MM_FW_CLASS(WKPreferences);
    
    namespace ak {
    namespace {
    using json_object = mm_json::object;
    using json_array = mm_json::array;
    mm_json num(std::uint64_t n) { return mm_json(mm_json_number{std::to_string(n)}); }
    const mm_json &field(const mm_json &o, const char *key) {
        static const mm_json missing;
        const auto *p = std::get_if<json_object>(&o.value);
        if (!p) return missing;
        auto it = p->find(key); return it == p->end() ? missing : it->second;
    }
    std::string text(const mm_json &j) {
        const auto *p = std::get_if<std::string>(&j.value); return p ? *p : std::string();
    }
    std::uint64_t integer(const mm_json &j) {
        const auto *n = std::get_if<mm_json_number>(&j.value);
        return n ? std::strtoull(n->text.c_str(), nullptr, 10) : 0;
    }
    bool truth(const mm_json &j) { const auto *b = std::get_if<bool>(&j.value); return b && *b; }
    id array() { return obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array")); }
    id dictionary() { return obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary")); }
    void dictionary_put(id d, const char *key, id value) {
        if (value) send(d, "setObject:forKey:", ret(value), ret(string(key)));
    }
    id from_json(const mm_json &j) {
        if (const auto *s = std::get_if<std::string>(&j.value)) return string(*s);
        if (const auto *b = std::get_if<bool>(&j.value))
            return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", *b));
        if (const auto *n = std::get_if<mm_json_number>(&j.value)) {
            if (n->text.find_first_of(".eE") != std::string::npos) return number(std::strtod(n->text.c_str(), nullptr));
            return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithLongLong:", std::strtoull(n->text.c_str(), nullptr, 10)));
        }
        if (const auto *a = std::get_if<json_array>(&j.value)) {
            id out = array(); for (const auto &v : *a) send(out, "addObject:", ret(from_json(v))); return out;
        }
        if (const auto *o = std::get_if<json_object>(&j.value)) {
            id out = dictionary();
            for (const auto &[key, v] : *o) send(out, "setObject:forKey:", ret(from_json(v)), ret(string(key)));
            return out;
        }
        return obj(send(reinterpret_cast<id>(cls("NSNull")), "null"));
    }
    id error(std::int64_t code, const std::string &description) {
        id info = dictionary(); dictionary_put(info, "NSLocalizedDescription", string(description));
        return obj(send(reinterpret_cast<id>(cls("NSError")), "errorWithDomain:code:userInfo:",
            ret(string("WKErrorDomain")), static_cast<std::uint64_t>(code), ret(info)));
    }
    struct object {
        mm_objc_object base;
        std::map<std::string, id> values;
        ~object() { for (const auto &[key, value] : values) release(value); }
    };
    id get(id self, const char *key) {
        auto &values = as<object>(self)->values;
        const auto it = values.find(key); return it == values.end() ? nullptr : it->second;
    }
    void put(id self, const char *key, id value) {
        auto &slot = as<object>(self)->values[key];
        retain(value); release(slot); slot = value;
    }
    id make_object(const char *name) { return obj(send(reinterpret_cast<id>(cls(name)), "new")); }
    std::uint64_t copy_object(id self, SEL, void *) {
        id copy = make_object(mm_objc_class_name(self->isa));
        for (const auto &[key, value] : as<object>(self)->values) put(copy, key.c_str(), value);
        return ret(copy);
    }
    void property(Class c, const char *key, const char *setter) {
        // Accessors use their actual selector as the key, and derive the setter key.
        mm_objc_add_method(c, key, MM_IMP(+[](id s, SEL sel) { return ret(get(s, sel)); }), "@16@0:8");
        mm_objc_add_method(c, setter, MM_IMP(+[](id s, SEL sel, id value) -> std::uint64_t {
            std::string key(sel + 3); key.pop_back();
            key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
            put(s, key.c_str(), value); return 0;
        }), "v24@0:8@16");
    }
    Class plain(const char *name, mm_objc_class *storage, mm_objc_class *meta) {
        Class c = mm_objc_define_class(name, cls("NSObject"), sizeof(object), storage, meta);
        register_lifecycle<object>(c);
        MM_METHODS(c, {"copyWithZone:", MM_IMP(copy_object), "@24@0:8^v16"});
        return c;
    }
    
    using dispatch_fn = void (*)(void *, void *, void *);
    using main_queue_fn = void *(*)();
    dispatch_fn dispatch = nullptr;
    main_queue_fn main_queue = nullptr;
    struct task { void (*run)(task *); virtual ~task() = default; };
    void run_task(std::uint64_t pointer) {
        std::unique_ptr<task> item(reinterpret_cast<task *>(pointer));
        autorelease_pool pool; item->run(item.get());
    }
    void post(task *item) {
        if (dispatch && main_queue) dispatch(main_queue(), item, reinterpret_cast<void *>(run_task));
        else { std::fprintf(stderr, "WebKit: Cocoa main queue is unavailable\n"); delete item; }
    }
    struct block_task : task {
        id block; std::vector<id> objects;
        block_task(id b, std::initializer_list<id> args) : block(retain(b)), objects(args) {
            for (id value : objects) retain(value);
            run = +[](task *t) {
                auto *self = static_cast<block_task *>(t);
                std::vector<std::uint64_t> words;
                for (id value : self->objects) words.push_back(ret(value));
                mm_objc_block_invoke(self->block, static_cast<int>(words.size()), words.data());
            };
        }
        ~block_task() { release(block); for (id value : objects) release(value); }
    };
    void complete(id block, std::initializer_list<id> args = {}) { if (block) post(new block_task(block, args)); }
    
    struct cookie_store {
        mm_objc_object base;
        std::vector<id> cookies;
        bool persistent = false;
        ~cookie_store() { for (id c : cookies) release(c); }
    };
    id cocoa_cookies() { return obj(send(reinterpret_cast<id>(cls("NSHTTPCookieStorage")), "sharedHTTPCookieStorage")); }
    bool same_cookie(id a, id b) {
        for (const char *key : {"name", "domain", "path"})
            if (to_utf8(obj(send(a, key))) != to_utf8(obj(send(b, key)))) return false;
        return true;
    }
    void save_cookie(id store, id cookie, bool remove) {
        if (!cookie) return;
        auto *s = as<cookie_store>(store);
        auto it = std::find_if(s->cookies.begin(), s->cookies.end(), [&](id c) { return same_cookie(c, cookie); });
        if (it != s->cookies.end()) { release(*it); s->cookies.erase(it); }
        if (!remove) s->cookies.push_back(retain(cookie));
        if (s->persistent) send(cocoa_cookies(), remove ? "deleteCookie:" : "setCookie:", ret(cookie));
    }
    mm_json encode_cookie(id cookie) {
        json_object out;
        for (const auto &[property, key] : {std::pair{"name", "Name"}, {"value", "Value"},
            {"domain", "Domain"}, {"path", "Path"}})
            out[key] = mm_json(to_utf8(obj(send(cookie, property))));
        out["Secure"] = mm_json(send(cookie, "isSecure") != 0);
        out["HTTPOnly"] = mm_json(send(cookie, "isHTTPOnly") != 0);
        if (id date = obj(send(cookie, "expiresDate"))) {
            // NSDate's interval is a floating return; read it from the runtime context.
            send(date, "timeIntervalSince1970");
            if (auto *ctx = api()->current_context()) {
                double time; std::memcpy(&time, &ctx->v[0][0], 8);
                out["Expires"] = mm_json(mm_json_number{std::to_string(time)});
            }
    tests/guest_webview.m:13:- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation {
    tests/guest_webview.m:21:- (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    tests/guest_webview.m:34:        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    tests/guest_webview.m:35:        check(configuration.websiteDataStore == WKWebsiteDataStore.defaultDataStore &&
    tests/guest_webview.m:39:        WKWebsiteDataStore *temporary = WKWebsiteDataStore.nonPersistentDataStore;
    tests/guest_webview.m:54:        WKWebView *view = [[WKWebView alloc] initWithFrame:NSMakeRect(5, 10, 400, 300) configuration:configuration];
    tests/guest_webview.m:77:    puts("WKWebView hierarchy, configuration, cookies and guest callbacks passed");
    work/roblox-import-audit.tsv:45:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _OBJC_CLASS_$_NSMutableURLRequest    nonlazy    no    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
    work/roblox-import-audit.tsv:56:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation    _OBJC_CLASS_$_NSURLRequest    nonlazy    no    /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
    work/roblox-import-audit.tsv:542:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebView    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
    work/roblox-import-audit.tsv:543:resolver    /mnt/media/machoman/work/venv/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit    _OBJC_CLASS_$_WKWebViewConfiguration    nonlazy    no    /System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
    ✓ • 250ms

## Activity

    $ ctest --test-dir /tmp/machoman-roblox-input-build -R '^guest_webview_' --output-on-failure
    Test project /tmp/machoman-roblox-input-build
        Start 73: guest_webview_arm64
    1/4 Test #73: guest_webview_arm64 ..............   Passed    0.37 sec
        Start 74: guest_webview_browser_arm64
    2/4 Test #74: guest_webview_browser_arm64 ......   Passed    0.92 sec
        Start 75: guest_webview_x86_64
    3/4 Test #75: guest_webview_x86_64 .............   Passed    0.11 sec
        Start 76: guest_webview_browser_x86_64
    4/4 Test #76: guest_webview_browser_x86_64 .....   Passed    0.62 sec
    
    100% tests passed out of 4
    
    Total Test time (real) =   2.02 sec
    ✓ • 1.91s

## User

можешь продолжить?
