add_executable(test_macho_parse test_macho_parse.cpp)
target_link_libraries(test_macho_parse PRIVATE machoman_core)

add_executable(test_translator test_translator.cpp)
target_link_libraries(test_translator PRIVATE machoman_core)

add_test(NAME macho_parse
         COMMAND test_macho_parse "${CMAKE_SOURCE_DIR}/third_party/samples/rg-arm64")
add_executable(test_runtime_arch test_runtime_arch.cpp)
add_executable(test_environment test_environment.cpp)
target_link_libraries(test_environment PRIVATE machoman_core)
add_test(NAME environment COMMAND test_environment "${CMAKE_CURRENT_BINARY_DIR}/environment-config-fixture")
target_link_libraries(test_runtime_arch PRIVATE machoman_core)
add_test(NAME runtime_arch COMMAND test_runtime_arch
    "${CMAKE_SOURCE_DIR}/third_party/samples/rg-arm64" "${CMAKE_CURRENT_BINARY_DIR}/runtime-arch-fixture")
set_tests_properties(runtime_arch PROPERTIES TIMEOUT 10)
add_test(NAME translator COMMAND test_translator)
add_test(NAME translator_no_avx2 COMMAND test_translator)
set_tests_properties(translator_no_avx2 PROPERTIES ENVIRONMENT "MACHOMAN_DISABLE_AVX2=1")
add_test(NAME translator_register_cache_disabled COMMAND test_translator --register-cache-only)
set_tests_properties(translator_register_cache_disabled PROPERTIES ENVIRONMENT "MACHOMAN_REGISTER_CACHE=0")
add_test(NAME translator_register_cache_watch COMMAND test_translator --register-cache-only)
set_tests_properties(translator_register_cache_watch PROPERTIES ENVIRONMENT "MACHOMAN_WATCH=0x1004;MACHOMAN_WATCH_LIMIT=1")

add_executable(test_platform_events test_platform_events.cpp)
target_include_directories(test_platform_events PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME platform_events COMMAND test_platform_events)

if(MM_WAYLAND_FOUND AND TARGET AppKit)
    pkg_check_modules(MM_WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
    if(MM_WAYLAND_SERVER_FOUND)
        add_executable(test_wayland_clipboard test_wayland_clipboard.cpp ../src/dylibs/AppKit/clipboard_wayland.cpp)
        target_link_libraries(test_wayland_clipboard PRIVATE PkgConfig::MM_WAYLAND_SERVER ${MM_WAYLAND_LIBRARIES} Threads::Threads)
        if(EXISTS "${MM_DATA_CONTROL_XML}")
            add_custom_command(OUTPUT "${MM_XDG_DIR}/ext-data-control-v1-server-protocol.h"
                COMMAND ${MM_WAYLAND_SCANNER} server-header "${MM_DATA_CONTROL_XML}" "${MM_XDG_DIR}/ext-data-control-v1-server-protocol.h"
                DEPENDS "${MM_DATA_CONTROL_XML}" VERBATIM)
            target_sources(test_wayland_clipboard PRIVATE "${MM_XDG_DIR}/ext-data-control-v1-server-protocol.h"
                "${MM_XDG_DIR}/ext-data-control-v1-client-protocol.h" "${MM_XDG_DIR}/ext-data-control-v1-protocol.c")
            target_include_directories(test_wayland_clipboard PRIVATE "${MM_XDG_DIR}")
            target_compile_definitions(test_wayland_clipboard PRIVATE MM_APPKIT_DATA_CONTROL=1)
        endif()
        add_test(NAME wayland_clipboard COMMAND test_wayland_clipboard)
        set_tests_properties(wayland_clipboard PROPERTIES TIMEOUT 20)
    endif()
endif()

add_executable(test_venv_install test_venv_install.cpp)
target_link_libraries(test_venv_install PRIVATE machoman_core)
add_test(NAME venv_install COMMAND test_venv_install "${CMAKE_CURRENT_BINARY_DIR}/install-fixture")
set_tests_properties(venv_install PROPERTIES TIMEOUT 30)

add_executable(test_metal_presentation test_metal_presentation.cpp)
add_test(NAME metal_presentation COMMAND test_metal_presentation)

if(TARGET machoman_audio_engine)
    add_executable(test_audio_engine test_audio_engine.cpp)
    target_link_libraries(test_audio_engine PRIVATE machoman_audio_engine Threads::Threads)
    add_test(NAME audio_engine COMMAND test_audio_engine)
    set_tests_properties(audio_engine PROPERTIES TIMEOUT 10)
endif()



add_executable(test_translation_cache test_translation_cache.cpp)
add_executable(test_native_menu test_native_menu.cpp ../src/dylibs/AppKit/native_menu.cpp)
target_link_libraries(test_native_menu PRIVATE machoman_core)
if(MM_PANGO_FOUND)
    target_link_libraries(test_native_menu PRIVATE PkgConfig::MM_PANGO)
    target_compile_definitions(test_native_menu PRIVATE MM_APPKIT_PANGO=1)
endif()
add_test(NAME native_menu COMMAND test_native_menu)
target_link_libraries(test_translation_cache PRIVATE machoman_core)
add_test(NAME translation_cache_write COMMAND test_translation_cache write "${CMAKE_CURRENT_BINARY_DIR}/cache-fixture/guest")
add_test(NAME translation_cache_read COMMAND test_translation_cache read "${CMAKE_CURRENT_BINARY_DIR}/cache-fixture/guest")
set_tests_properties(translation_cache_write PROPERTIES FIXTURES_SETUP translation_cache)
set_tests_properties(translation_cache_read PROPERTIES FIXTURES_REQUIRED translation_cache)
add_test(NAME translation_cache_disabled COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_TRANSLATION_CACHE=0
    $<TARGET_FILE:test_translation_cache> disabled "${CMAKE_CURRENT_BINARY_DIR}/cache-fixture/guest")

if(PNG_FOUND AND TARGET AppKit)
    add_executable(test_application_icon test_application_icon.cpp "${CMAKE_SOURCE_DIR}/src/dylibs/AppKit/icon_image.cpp")
    target_include_directories(test_application_icon PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_definitions(test_application_icon PRIVATE MM_APPKIT_PNG=1)
    target_link_libraries(test_application_icon PRIVATE PNG::PNG)
    add_test(NAME application_icon COMMAND test_application_icon "${CMAKE_CURRENT_BINARY_DIR}/icon-fixture")
endif()

add_executable(test_simd_helpers test_simd_helpers.cpp)
target_link_libraries(test_simd_helpers PRIVATE machoman_core)
add_test(NAME simd_helpers COMMAND test_simd_helpers "${CMAKE_CURRENT_SOURCE_DIR}/simd_vectors.txt")
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
endif()

# Optional execution checks for the Darwin ABI, compiled with a local SDK.
set(MM_TEST_MACOS_SDK "" CACHE PATH "macOS SDK used to cross-compile guest runtime tests")
if(TARGET swiftCore)
    add_subdirectory(swiftCore)
endif()
if(MM_TEST_MACOS_SDK AND TARGET Foundation)
    find_program(MM_TEST_CLANG clang REQUIRED)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(MM_INSTALL_NAME_TOOL llvm-install-name-tool)
    if(Python3_Interpreter_FOUND AND MM_INSTALL_NAME_TOOL)
        add_test(NAME bottle_relocation COMMAND ${Python3_EXECUTABLE} -B
            "${CMAKE_CURRENT_SOURCE_DIR}/test_bottle_relocation.py"
            "${CMAKE_CURRENT_BINARY_DIR}/bottle-relocation-fixture" $<TARGET_FILE:machoman>
            "${MM_TEST_MACOS_SDK}" "${MM_TEST_CLANG}" "${MM_INSTALL_NAME_TOOL}"
            "${CMAKE_SOURCE_DIR}/scripts/relocate-homebrew-bottles.py")
        set_tests_properties(bottle_relocation PROPERTIES TIMEOUT 45)
    endif()
    set(MM_ENV_VENV "${CMAKE_CURRENT_BINARY_DIR}/environment-venv")
    add_custom_command(OUTPUT "${MM_ENV_VENV}/machoman-venv.json"
        COMMAND ${CMAKE_COMMAND} -DMACHOMAN=$<TARGET_FILE:machoman> -DVENV=${MM_ENV_VENV}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare_environment_venv.cmake"
        DEPENDS machoman prepare_environment_venv.cmake VERBATIM)
    add_library(environment_probe SHARED environment_probe.cpp)
    set_target_properties(environment_probe PROPERTIES PREFIX "" OUTPUT_NAME "libenvironment_probe.dylib"
        SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${MM_ENV_VENV}/usr/lib")
    foreach(MM_ENV_ARCH IN ITEMS arm64 x86_64)
        set(MM_ENV_BINARY "${MM_ENV_VENV}/bin/environment-${MM_ENV_ARCH}")
        add_custom_command(OUTPUT "${MM_ENV_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_ENV_ARCH}-apple-macos12 -isysroot "${MM_TEST_MACOS_SDK}"
                -fuse-ld=lld -O2 -framework Foundation -Wl,-undefined,dynamic_lookup
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_environment.m" -o "${MM_ENV_BINARY}"
            DEPENDS "${MM_ENV_VENV}/machoman-venv.json" guest_environment.m VERBATIM)
        add_custom_target(guest_environment_${MM_ENV_ARCH}_binary ALL DEPENDS "${MM_ENV_BINARY}")
        foreach(MM_ENV_CHILD IN ITEMS arm64 x86_64)
            add_test(NAME guest_environment_${MM_ENV_ARCH}_${MM_ENV_CHILD}
                COMMAND $<TARGET_FILE:machoman> run -e MM_CLI_VALUE=cli "${MM_ENV_VENV}"
                    environment-${MM_ENV_ARCH} "${MM_ENV_VENV}/bin/environment-${MM_ENV_CHILD}")
            set_tests_properties(guest_environment_${MM_ENV_ARCH}_${MM_ENV_CHILD} PROPERTIES TIMEOUT 30
                ENVIRONMENT "MM_HOST_CONTROL=host;MM_VENV_MASK=host;DYLD_INSERT_LIBRARIES=/usr/lib/libenvironment_probe.dylib"
                PASS_REGULAR_EXPRESSION "SDK live environment, Foundation, putenv, inheritance, empty envp and argv0 passed")
        endforeach()
    endforeach()
    set(MM_SMOKE_VENV "${CMAKE_CURRENT_BINARY_DIR}/guest-venv")
    foreach(MM_PROC_ARCH IN ITEMS arm64 x86_64)
        set(MM_PROC_BINARY "${MM_SMOKE_VENV}/bin/proc-info-${MM_PROC_ARCH}")
        add_custom_command(OUTPUT "${MM_PROC_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_PROC_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_proc_info.c" -o "${MM_PROC_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_proc_info.c VERBATIM)
        add_custom_target(guest_proc_info_${MM_PROC_ARCH}_binary ALL DEPENDS "${MM_PROC_BINARY}")
        add_test(NAME guest_proc_info_${MM_PROC_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" proc-info-${MM_PROC_ARCH})
        set_tests_properties(guest_proc_info_${MM_PROC_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "Darwin proc_pidinfo descriptor enumeration, types and bounds passed")
    endforeach()
    foreach(MM_LIMIT_ARCH IN ITEMS arm64 x86_64)
        set(MM_LIMIT_BINARY "${MM_SMOKE_VENV}/bin/resource-limits-${MM_LIMIT_ARCH}")
        add_custom_command(OUTPUT "${MM_LIMIT_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_LIMIT_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_resource_limits.c" -o "${MM_LIMIT_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_resource_limits.c VERBATIM)
        add_custom_target(guest_resource_limits_${MM_LIMIT_ARCH}_binary ALL DEPENDS "${MM_LIMIT_BINARY}")
        add_test(NAME guest_resource_limits_${MM_LIMIT_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" resource-limits-${MM_LIMIT_ARCH})
        set_tests_properties(guest_resource_limits_${MM_LIMIT_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "Darwin resource numbers, infinity, descriptor enforcement and memory limit isolation passed")
    endforeach()
    if(MM_WAYLAND_FOUND AND TARGET AppKit)
        foreach(MM_GL_ARCH IN ITEMS arm64 x86_64)
            set(MM_GL_BINARY "${MM_SMOKE_VENV}/bin/opengl-threads-${MM_GL_ARCH}")
            add_custom_command(OUTPUT "${MM_GL_BINARY}"
                COMMAND ${MM_TEST_CLANG} --target=${MM_GL_ARCH}-apple-macos12
                    -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework AppKit -framework OpenGL
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_opengl_threads.m" -o "${MM_GL_BINARY}"
                DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_opengl_threads.m VERBATIM)
            add_custom_target(guest_opengl_threads_${MM_GL_ARCH}_binary ALL DEPENDS "${MM_GL_BINARY}")
            add_test(NAME guest_opengl_threads_${MM_GL_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" opengl-threads-${MM_GL_ARCH})
            set_tests_properties(guest_opengl_threads_${MM_GL_ARCH} PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
        endforeach()
    endif()
    foreach(MM_DISPATCH_DATA_ARCH IN ITEMS arm64 x86_64)
        set(MM_DISPATCH_DATA_BINARY "${MM_SMOKE_VENV}/bin/dispatch-data-${MM_DISPATCH_DATA_ARCH}")
        add_custom_command(OUTPUT "${MM_DISPATCH_DATA_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_DISPATCH_DATA_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -fblocks -O2 -framework CoreFoundation
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_dispatch_data.c" -o "${MM_DISPATCH_DATA_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_dispatch_data.c VERBATIM)
        add_custom_target(guest_dispatch_data_${MM_DISPATCH_DATA_ARCH}_binary ALL DEPENDS "${MM_DISPATCH_DATA_BINARY}")
        add_test(NAME guest_dispatch_data_${MM_DISPATCH_DATA_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" dispatch-data-${MM_DISPATCH_DATA_ARCH})
        set_tests_properties(guest_dispatch_data_${MM_DISPATCH_DATA_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "SDK dispatch data mapping, regions, ownership, blocks and destructors passed")
    endforeach()
    foreach(MM_JUMP_ARCH IN ITEMS arm64 x86_64)
        set(MM_JUMP_BINARY "${MM_SMOKE_VENV}/bin/setjmp-${MM_JUMP_ARCH}")
        add_custom_command(OUTPUT "${MM_JUMP_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_JUMP_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_setjmp.c" -o "${MM_JUMP_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_setjmp.c VERBATIM)
        add_custom_target(guest_setjmp_${MM_JUMP_ARCH}_binary ALL DEPENDS "${MM_JUMP_BINARY}")
        add_test(NAME guest_setjmp_${MM_JUMP_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" setjmp-${MM_JUMP_ARCH})
        set_tests_properties(guest_setjmp_${MM_JUMP_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "SDK setjmp bounds, register restoration, nested jumps and signal masks passed")
    endforeach()
    foreach(MM_FOCUS_ARCH IN ITEMS arm64 x86_64)
        set(MM_PASTEBOARD_BINARY "${MM_SMOKE_VENV}/bin/pasteboard-${MM_FOCUS_ARCH}")
        add_custom_command(OUTPUT "${MM_PASTEBOARD_BINARY}"
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
            COMMAND ${CMAKE_COMMAND} -E env --unset=WAYLAND_DISPLAY --unset=DISPLAY
                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" text-view-${MM_FOCUS_ARCH})
        set_tests_properties(guest_text_view_${MM_FOCUS_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "NSTextView field editor, guest text callbacks and UTF-16 range ABI passed")
        set(MM_FOCUS_BINARY "${MM_SMOKE_VENV}/bin/focus-reentrancy-${MM_FOCUS_ARCH}")
        add_custom_command(OUTPUT "${MM_FOCUS_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_FOCUS_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -framework AppKit
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_focus_reentrancy.m" -o "${MM_FOCUS_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_focus_reentrancy.m VERBATIM)
        add_custom_target(guest_focus_reentrancy_${MM_FOCUS_ARCH}_binary ALL DEPENDS "${MM_FOCUS_BINARY}")
        add_test(NAME guest_focus_reentrancy_${MM_FOCUS_ARCH}
            COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" focus-reentrancy-${MM_FOCUS_ARCH})
        set_tests_properties(guest_focus_reentrancy_${MM_FOCUS_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "AppKit focus rollback and reentrant responder selection passed")
    endforeach()
    add_executable(test_import_audit test_import_audit.cpp)
    target_link_libraries(test_import_audit PRIVATE machoman_core)
    foreach(MM_AUDIT_ARCH IN ITEMS arm64 x86_64)
        set(MM_AUDIT_BINARY "${MM_SMOKE_VENV}/bin/import-audit-${MM_AUDIT_ARCH}")
        add_custom_command(OUTPUT "${MM_AUDIT_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_AUDIT_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -Wl,-undefined,dynamic_lookup
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_import_audit.c" -o "${MM_AUDIT_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_import_audit.c VERBATIM)
        add_custom_target(guest_import_audit_${MM_AUDIT_ARCH}_binary ALL DEPENDS "${MM_AUDIT_BINARY}")
        add_test(NAME import_audit_${MM_AUDIT_ARCH}
            COMMAND test_import_audit "${MM_SMOKE_VENV}" import-audit-${MM_AUDIT_ARCH})
        set_tests_properties(import_audit_${MM_AUDIT_ARCH} PROPERTIES TIMEOUT 20
            ENVIRONMENT "MACHOMAN_DYLIB_PATH=$<TARGET_FILE_DIR:machoman>/dylibs")
    endforeach()
    foreach(MM_ERASE_ARCH IN ITEMS arm64 x86_64)
        set(MM_ERASE_BINARY "${MM_SMOKE_VENV}/bin/memset-s-${MM_ERASE_ARCH}")
        add_custom_command(OUTPUT "${MM_ERASE_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_ERASE_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_memset_s.c" -o "${MM_ERASE_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_memset_s.c VERBATIM)
        add_custom_target(guest_memset_s_${MM_ERASE_ARCH}_binary ALL DEPENDS "${MM_ERASE_BINARY}")
        add_test(NAME guest_memset_s_${MM_ERASE_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" memset-s-${MM_ERASE_ARCH})
        set_tests_properties(guest_memset_s_${MM_ERASE_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "SDK memset_s bounds, error erasure and Darwin error codes passed")
    endforeach()
    foreach(MM_LOCK_ARCH IN ITEMS arm64 x86_64)
        set(MM_LOCK_BINARY "${MM_SMOKE_VENV}/bin/file-locks-${MM_LOCK_ARCH}")
        add_custom_command(OUTPUT "${MM_LOCK_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_LOCK_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_file_locks.c" -o "${MM_LOCK_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_file_locks.c VERBATIM)
        add_custom_target(guest_file_locks_${MM_LOCK_ARCH}_binary ALL DEPENDS "${MM_LOCK_BINARY}")
        add_test(NAME guest_file_locks_${MM_LOCK_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" file-locks-${MM_LOCK_ARCH})
        set_tests_properties(guest_file_locks_${MM_LOCK_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "SDK fcntl record and OFD locks, conflicts and cross-process wakeup passed")
    endforeach()
    foreach(MM_NOTIFICATION_ARCH IN ITEMS arm64 x86_64)
        set(MM_NOTIFICATION_BINARY "${MM_SMOKE_VENV}/bin/notifications-${MM_NOTIFICATION_ARCH}")
        add_custom_command(OUTPUT "${MM_NOTIFICATION_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_NOTIFICATION_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -fblocks -framework Foundation -framework CoreFoundation
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_notifications.m" -o "${MM_NOTIFICATION_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_notifications.m VERBATIM)
        add_custom_target(guest_notifications_${MM_NOTIFICATION_ARCH}_binary ALL DEPENDS "${MM_NOTIFICATION_BINARY}")
        add_test(NAME guest_notifications_${MM_NOTIFICATION_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" notifications-${MM_NOTIFICATION_ARCH})
        set_tests_properties(guest_notifications_${MM_NOTIFICATION_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "Cocoa notification queues and reentrant observer delivery passed")
    endforeach()
    foreach(MM_COOKIE_ARCH IN ITEMS arm64 x86_64)
        set(MM_COOKIE_BINARY "${MM_SMOKE_VENV}/bin/http-cookies-${MM_COOKIE_ARCH}")
        add_custom_command(OUTPUT "${MM_COOKIE_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_COOKIE_ARCH}-apple-macos12
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
    foreach(MM_WEBVIEW_ARCH IN ITEMS arm64 x86_64)
        set(MM_WEBVIEW_BINARY "${MM_SMOKE_VENV}/bin/webview-${MM_WEBVIEW_ARCH}")
        add_custom_command(OUTPUT "${MM_WEBVIEW_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_WEBVIEW_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -fblocks -framework AppKit -framework WebKit
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_webview.m" -o "${MM_WEBVIEW_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_webview.m VERBATIM)
        add_custom_target(guest_webview_${MM_WEBVIEW_ARCH}_binary ALL DEPENDS "${MM_WEBVIEW_BINARY}")
        add_test(NAME guest_webview_${MM_WEBVIEW_ARCH}
            COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
                $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" webview-${MM_WEBVIEW_ARCH})
        set_tests_properties(guest_webview_${MM_WEBVIEW_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "WKWebView hierarchy, configuration, cookies and guest callbacks passed")
        if(TARGET machoman-webview)
            add_test(NAME guest_webview_browser_${MM_WEBVIEW_ARCH}
                COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1 QT_QPA_PLATFORM=offscreen
                    $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" webview-${MM_WEBVIEW_ARCH} browser)
            set_tests_properties(guest_webview_browser_${MM_WEBVIEW_ARCH} PROPERTIES TIMEOUT 25
                PASS_REGULAR_EXPRESSION "WKWebView hierarchy, configuration, cookies and guest callbacks passed")
            add_test(NAME guest_webview_html_${MM_WEBVIEW_ARCH}
                COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1 QT_QPA_PLATFORM=offscreen
                    $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" webview-${MM_WEBVIEW_ARCH} browser html)
            set_tests_properties(guest_webview_html_${MM_WEBVIEW_ARCH} PROPERTIES TIMEOUT 25
                PASS_REGULAR_EXPRESSION "WKWebView hierarchy, configuration, cookies and guest callbacks passed")
        endif()
    endforeach()
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
        add_test(NAME guest_uname_${MM_PASSWD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" uname-${MM_PASSWD_ARCH})
        set_tests_properties(guest_uname_${MM_PASSWD_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "Darwin uname SDK layout, kernel release, machine identity and bounds passed")
        set(MM_PASSWD_BINARY "${MM_SMOKE_VENV}/bin/passwd-${MM_PASSWD_ARCH}")
        add_custom_command(OUTPUT "${MM_PASSWD_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_PASSWD_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_passwd.c" -o "${MM_PASSWD_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_passwd.c VERBATIM)
        add_custom_target(guest_passwd_${MM_PASSWD_ARCH}_binary ALL DEPENDS "${MM_PASSWD_BINARY}")
        add_test(NAME guest_passwd_${MM_PASSWD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" passwd-${MM_PASSWD_ARCH})
        set_tests_properties(guest_passwd_${MM_PASSWD_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "Darwin passwd layout, caller buffers, user lookup and enumeration passed")
    endforeach()
    foreach(MM_THREAD_ARCH IN ITEMS arm64 x86_64)
        set(MM_ATFORK_BINARY "${MM_SMOKE_VENV}/bin/atfork-${MM_THREAD_ARCH}")
        add_custom_command(OUTPUT "${MM_ATFORK_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_THREAD_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_atfork.c" -o "${MM_ATFORK_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_atfork.c VERBATIM)
        add_custom_target(guest_atfork_${MM_THREAD_ARCH}_binary ALL DEPENDS "${MM_ATFORK_BINARY}")
        add_test(NAME guest_atfork_${MM_THREAD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" atfork-${MM_THREAD_ARCH})
        set_tests_properties(guest_atfork_${MM_THREAD_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "SDK atfork guest callbacks, ordering, TLS identity and parent/child isolation passed")
        set(MM_HOST_BINARY "${MM_SMOKE_VENV}/bin/mach-host-${MM_THREAD_ARCH}")
        add_custom_command(OUTPUT "${MM_HOST_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_THREAD_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_mach_host.c" -o "${MM_HOST_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_mach_host.c VERBATIM)
        add_custom_target(guest_mach_host_${MM_THREAD_ARCH}_binary ALL DEPENDS "${MM_HOST_BINARY}")
        add_test(NAME guest_mach_host_${MM_THREAD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" mach-host-${MM_THREAD_ARCH})
        set_tests_properties(guest_mach_host_${MM_THREAD_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "SDK Mach host page size, hardware, VM revisions, CPU/load counters and bounds passed")
        set(MM_TIME_BINARY "${MM_SMOKE_VENV}/bin/locale-time-${MM_THREAD_ARCH}")
        add_custom_command(OUTPUT "${MM_TIME_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_THREAD_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_locale_time.c" -o "${MM_TIME_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_locale_time.c VERBATIM)
        add_custom_target(guest_locale_time_${MM_THREAD_ARCH}_binary ALL DEPENDS "${MM_TIME_BINARY}")
        add_test(NAME guest_locale_time_${MM_THREAD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" locale-time-${MM_THREAD_ARCH})
        set_tests_properties(guest_locale_time_${MM_THREAD_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "SDK date formatting/parsing, null and global locales, wide output and caller state passed")
        set(MM_INET_BINARY "${MM_SMOKE_VENV}/bin/inet-addresses-${MM_THREAD_ARCH}")
        add_custom_command(OUTPUT "${MM_INET_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_THREAD_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_inet_addresses.c" -o "${MM_INET_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_inet_addresses.c VERBATIM)
        add_custom_target(guest_inet_addresses_${MM_THREAD_ARCH}_binary ALL DEPENDS "${MM_INET_BINARY}")
        add_test(NAME guest_inet_addresses_${MM_THREAD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" inet-addresses-${MM_THREAD_ARCH})
        set_tests_properties(guest_inet_addresses_${MM_THREAD_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "SDK inet IPv4/IPv6 literals, DNS classification, network bytes and Darwin errno passed")
        set(MM_THREAD_BINARY "${MM_SMOKE_VENV}/bin/thread-destructors-${MM_THREAD_ARCH}")
        add_custom_command(OUTPUT "${MM_THREAD_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_THREAD_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_thread_destructors.c" -o "${MM_THREAD_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_thread_destructors.c VERBATIM)
        add_custom_target(guest_thread_destructors_${MM_THREAD_ARCH}_binary ALL DEPENDS "${MM_THREAD_BINARY}")
        add_test(NAME guest_thread_destructors_${MM_THREAD_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" thread-destructors-${MM_THREAD_ARCH})
        set_tests_properties(guest_thread_destructors_${MM_THREAD_ARCH} PROPERTIES TIMEOUT 15
            PASS_REGULAR_EXPRESSION "SDK pthread key width, thread identity, native GS, TLV lifetime and destructor iterations passed")
    endforeach()
    set(MM_NATIVE_SIGNALS "${MM_SMOKE_VENV}/bin/native-signals")
    add_custom_command(OUTPUT "${MM_NATIVE_SIGNALS}"
        COMMAND ${MM_TEST_CLANG} --target=x86_64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -mno-red-zone
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_signals.c" -o "${MM_NATIVE_SIGNALS}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_signals.c VERBATIM)
    add_custom_target(guest_native_signals_binary ALL DEPENDS "${MM_NATIVE_SIGNALS}")
    add_test(NAME guest_native_signals COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" native-signals)
    set_tests_properties(guest_native_signals PROPERTIES TIMEOUT 10
        PASS_REGULAR_EXPRESSION "Native Darwin signals, nested handlers, context edits, red zone and FPU restoration passed")
    foreach(MM_ULOCK_ARCH IN ITEMS arm64 x86_64)
        set(MM_ULOCK_BINARY "${MM_SMOKE_VENV}/bin/ulock-${MM_ULOCK_ARCH}")
        add_custom_command(OUTPUT "${MM_ULOCK_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_ULOCK_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_ulock.c" -o "${MM_ULOCK_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_ulock.c VERBATIM)
        add_custom_target(guest_ulock_${MM_ULOCK_ARCH}_binary ALL DEPENDS "${MM_ULOCK_BINARY}")
        add_test(NAME guest_ulock_${MM_ULOCK_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" ulock-${MM_ULOCK_ARCH})
        set_tests_properties(guest_ulock_${MM_ULOCK_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "Darwin ulock compare, deadlines, errno and multithreaded wake passed")
    endforeach()
    foreach(MM_XATTR_ARCH IN ITEMS arm64 x86_64)
        set(MM_XATTR_BINARY "${MM_SMOKE_VENV}/bin/xattr-${MM_XATTR_ARCH}")
        add_custom_command(OUTPUT "${MM_XATTR_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_XATTR_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_xattr.c" -o "${MM_XATTR_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_xattr.c VERBATIM)
        add_custom_target(guest_xattr_${MM_XATTR_ARCH}_binary ALL DEPENDS "${MM_XATTR_BINARY}")
        add_test(NAME guest_xattr_${MM_XATTR_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" xattr-${MM_XATTR_ARCH})
        set_tests_properties(guest_xattr_${MM_XATTR_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "Darwin xattr names, binary values, flags, listing and errno passed")
    endforeach()
    foreach(MM_PORT_ARCH IN ITEMS arm64 x86_64)
        set(MM_PORT_BINARY "${MM_SMOKE_VENV}/bin/mach-ports-${MM_PORT_ARCH}")
        add_custom_command(OUTPUT "${MM_PORT_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_PORT_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_mach_ports.c" -o "${MM_PORT_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_mach_ports.c VERBATIM)
        add_custom_target(guest_mach_ports_${MM_PORT_ARCH}_binary ALL DEPENDS "${MM_PORT_BINARY}")
        add_test(NAME guest_mach_ports_${MM_PORT_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" mach-ports-${MM_PORT_ARCH})
        set_tests_properties(guest_mach_ports_${MM_PORT_ARCH} PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "Mach local port rights, messages, timeout and destruction passed")
    endforeach()
    foreach(MM_EXCHANGE_ARCH IN ITEMS arm64 x86_64)
        set(MM_NOTIFICATION_BINARY "${MM_SMOKE_VENV}/bin/mach-notifications-${MM_EXCHANGE_ARCH}")
        add_custom_command(OUTPUT "${MM_NOTIFICATION_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_EXCHANGE_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_mach_notifications.c" -o "${MM_NOTIFICATION_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_mach_notifications.c VERBATIM)
        add_custom_target(guest_mach_notifications_${MM_EXCHANGE_ARCH}_binary ALL DEPENDS "${MM_NOTIFICATION_BINARY}")
        add_test(NAME guest_mach_notifications_${MM_EXCHANGE_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" mach-notifications-${MM_EXCHANGE_ARCH})
        set_tests_properties(guest_mach_notifications_${MM_EXCHANGE_ARCH} PROPERTIES TIMEOUT 15
            PASS_REGULAR_EXPRESSION "SDK Mach notifications, subscription replacement, dead references, backup rights and queue capacity passed")
        set(MM_EXCHANGE_BINARY "${MM_SMOKE_VENV}/bin/mach-exchange-${MM_EXCHANGE_ARCH}")
        add_custom_command(OUTPUT "${MM_EXCHANGE_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_EXCHANGE_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_mach_exchange.c" -o "${MM_EXCHANGE_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_mach_exchange.c VERBATIM)
        add_custom_target(guest_mach_exchange_${MM_EXCHANGE_ARCH}_binary ALL DEPENDS "${MM_EXCHANGE_BINARY}")
        foreach(MM_EXCHANGE_CHILD_ARCH IN ITEMS arm64 x86_64)
            add_test(NAME guest_mach_exchange_${MM_EXCHANGE_ARCH}_${MM_EXCHANGE_CHILD_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" mach-exchange-${MM_EXCHANGE_ARCH}
                    "${MM_SMOKE_VENV}/bin/mach-exchange-${MM_EXCHANGE_CHILD_ARCH}")
            set_tests_properties(guest_mach_exchange_${MM_EXCHANGE_ARCH}_${MM_EXCHANGE_CHILD_ARCH} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "SDK Mach cross-process descriptors, OOL memory, shared entries, port sets, trailers and replies passed")
        endforeach()
    endforeach()
    foreach(MM_PROCESS_ARCH IN ITEMS arm64 x86_64)
        set(MM_PROCESS_BINARY "${MM_SMOKE_VENV}/bin/process-shared-${MM_PROCESS_ARCH}")
        add_custom_command(OUTPUT "${MM_PROCESS_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=${MM_PROCESS_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_process_shared.c" -o "${MM_PROCESS_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_process_shared.c VERBATIM)
        add_custom_target(guest_process_shared_${MM_PROCESS_ARCH}_binary ALL DEPENDS "${MM_PROCESS_BINARY}")
        foreach(MM_CHILD_ARCH IN ITEMS arm64 x86_64)
            add_test(NAME guest_process_shared_${MM_PROCESS_ARCH}_${MM_CHILD_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" process-shared-${MM_PROCESS_ARCH}
                    "${MM_SMOKE_VENV}/bin/process-shared-${MM_CHILD_ARCH}")
            set_tests_properties(guest_process_shared_${MM_PROCESS_ARCH}_${MM_CHILD_ARCH} PROPERTIES TIMEOUT 15
                PASS_REGULAR_EXPRESSION "Separate guest processes, shared memory, file actions and lifetime passed")
            add_test(NAME guest_fork_exec_${MM_PROCESS_ARCH}_${MM_CHILD_ARCH}
                COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" process-shared-${MM_PROCESS_ARCH}
                    "${MM_SMOKE_VENV}/bin/process-shared-${MM_CHILD_ARCH}" --fork-parent)
            set_tests_properties(guest_fork_exec_${MM_PROCESS_ARCH}_${MM_CHILD_ARCH} PROPERTIES TIMEOUT 15
                PASS_REGULAR_EXPRESSION "Separate guest processes, shared memory, file actions and lifetime passed")
        endforeach()
    endforeach()
    add_custom_command(OUTPUT "${MM_NATIVE_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=x86_64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_smoke.c" -o "${MM_NATIVE_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_smoke.c VERBATIM)
    add_custom_target(guest_native_smoke_binary ALL DEPENDS "${MM_NATIVE_BINARY}")
    add_test(NAME guest_native_smoke COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" native-smoke)
    set_tests_properties(guest_native_smoke PROPERTIES TIMEOUT 20
        PASS_REGULAR_EXPRESSION "native x86_64 calls, callbacks, varargs, dlsym and TLS passed")
    set(MM_NATIVE_INTERPOSER "${MM_SMOKE_VENV}/usr/lib/libnative-interposer.dylib")
    set(MM_NATIVE_INTERPOSE "${MM_SMOKE_VENV}/bin/native-interpose")
    add_custom_command(OUTPUT "${MM_NATIVE_INTERPOSER}"
        COMMAND ${MM_TEST_CLANG} --target=x86_64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2 -dynamiclib
            -Wl,-install_name,@rpath/libnative-interposer.dylib
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_interposer.c" -o "${MM_NATIVE_INTERPOSER}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_native_interposer.c VERBATIM)
    add_custom_command(OUTPUT "${MM_NATIVE_INTERPOSE}"
        COMMAND ${MM_TEST_CLANG} --target=x86_64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
            -Wl,-rpath,@executable_path/../usr/lib "${MM_NATIVE_INTERPOSER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_native_interpose.c" -o "${MM_NATIVE_INTERPOSE}"
        DEPENDS "${MM_NATIVE_INTERPOSER}" guest_native_interpose.c VERBATIM)
    add_custom_target(guest_native_interpose_binary ALL DEPENDS "${MM_NATIVE_INTERPOSE}")
    add_test(NAME guest_native_interpose COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" native-interpose)
    set_tests_properties(guest_native_interpose PROPERTIES TIMEOUT 20
        PASS_REGULAR_EXPRESSION "native interposition and original-function fallback passed")
    set(MM_RESOURCE_BINARY "${MM_SMOKE_VENV}/bin/resource-stubs")
    add_custom_command(OUTPUT "${MM_RESOURCE_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -fblocks -O2
            -framework Foundation -framework AVFoundation
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_resource_stubs.m" -o "${MM_RESOURCE_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_resource_stubs.m VERBATIM)
    add_custom_target(guest_resource_stubs_binary ALL DEPENDS "${MM_RESOURCE_BINARY}")
    add_test(NAME guest_resource_stubs COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" resource-stubs)
    set_tests_properties(guest_resource_stubs PROPERTIES TIMEOUT 20
        PASS_REGULAR_EXPRESSION "NSURL resources and typed dynamic stubs passed")
    set(MM_SMOKE_BINARY "${MM_SMOKE_VENV}/bin/runtime-smoke")
    add_custom_command(OUTPUT "${MM_SMOKE_VENV}/machoman-venv.json"
        COMMAND ${CMAKE_COMMAND} -DMACHOMAN=$<TARGET_FILE:machoman>
            -DVENV=${MM_SMOKE_VENV} -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare_guest_venv.cmake"
        VERBATIM)
    add_custom_command(OUTPUT "${MM_SMOKE_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -fblocks -framework AppKit -framework Carbon -framework AVFoundation -framework OpenGL -framework CoreText
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_runtime_smoke.m" -o "${MM_SMOKE_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_runtime_smoke.m VERBATIM)
    add_custom_target(guest_runtime_smoke_binary ALL DEPENDS "${MM_SMOKE_BINARY}")
    add_test(NAME guest_runtime_smoke
        COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_OSLOG=1
            MACHOMAN_APPKIT_HEADLESS=1
            $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" runtime-smoke)
    set_tests_properties(guest_runtime_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "os_log\\[runtime-smoke:owned-strings type=1\\]"
        TIMEOUT 30)
    set(MM_KVO_SMOKE_BINARY "${MM_SMOKE_VENV}/bin/kvo-smoke")
    add_custom_command(OUTPUT "${MM_KVO_SMOKE_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -fblocks -framework AppKit
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_kvo_smoke.m" -o "${MM_KVO_SMOKE_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_kvo_smoke.m VERBATIM)
    add_custom_target(guest_kvo_smoke_binary ALL DEPENDS "${MM_KVO_SMOKE_BINARY}")
    add_test(NAME guest_kvo_smoke
        COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1 MACHOMAN_STRICT_SELECTORS=1
            $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" kvo-smoke)
    set_tests_properties(guest_kvo_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "KVO window attachment.*weak lifetime passed" TIMEOUT 30)
    set(MM_CXX_SMOKE_BINARY "${MM_SMOKE_VENV}/bin/cxx-smoke")
    add_custom_command(OUTPUT "${MM_CXX_SMOKE_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -fblocks
            -framework AppKit -lc++
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_cxx_smoke.mm" -o "${MM_CXX_SMOKE_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_cxx_smoke.mm VERBATIM)
    add_custom_target(guest_cxx_smoke_binary ALL DEPENDS "${MM_CXX_SMOKE_BINARY}")
    add_test(NAME guest_cxx_smoke
        COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
            $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" cxx-smoke)
    set_tests_properties(guest_cxx_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "guest Objective-C\\+\\+ construction and destruction passed"
        TIMEOUT 30)

    add_library(audio_null_probe SHARED audio_null_probe.cpp)
    target_include_directories(audio_null_probe PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(audio_null_probe PRIVATE machoman_audio_engine Threads::Threads)
    set_target_properties(audio_null_probe PROPERTIES
        PREFIX "lib" OUTPUT_NAME "audio_null_probe.dylib" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${MM_SMOKE_VENV}/usr/lib")
    set(MM_AUDIO_SMOKE_BINARY "${MM_SMOKE_VENV}/bin/audio-null-smoke")
    add_custom_command(OUTPUT "${MM_AUDIO_SMOKE_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12
            -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
            -L "${CMAKE_CURRENT_SOURCE_DIR}" -laudio_null_probe
            -Wl,-rpath,@executable_path/../usr/lib -framework CoreAudio
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_audio_null.c" -o "${MM_AUDIO_SMOKE_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_audio_null.c libaudio_null_probe.tbd VERBATIM)
    add_custom_target(guest_audio_null_binary ALL DEPENDS "${MM_AUDIO_SMOKE_BINARY}")
    add_dependencies(guest_audio_null_binary audio_null_probe)
    add_test(NAME guest_audio_null
        COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" audio-null-smoke)
    set_tests_properties(guest_audio_null PROPERTIES
        PASS_REGULAR_EXPRESSION "CoreAudio ARM64 ABI, sine samples.*fault recovery passed"
        TIMEOUT 30)
endif()

if(MM_GIO_FOUND)
    add_executable(test_native_dialog test_native_dialog.cpp ../src/dylibs/AppKit/native_dialog.cpp)
    target_compile_definitions(test_native_dialog PRIVATE MM_APPKIT_GIO=1)
    target_link_libraries(test_native_dialog PRIVATE PkgConfig::MM_GIO)
    add_test(NAME native_dialog COMMAND test_native_dialog)
    set_tests_properties(native_dialog PROPERTIES TIMEOUT 15)
endif()

add_executable(test_nib_archive test_nib_archive.cpp ../src/dylibs/AppKit/nib_archive.cpp)
add_test(NAME nib_archive COMMAND test_nib_archive)
if(MM_TEST_MACOS_SDK AND TARGET AppKit)
    set(MM_DIALOG_BUNDLE "${MM_SMOKE_VENV}/Applications/DialogSmoke.app/Contents")
    set(MM_DIALOG_BINARY "${MM_DIALOG_BUNDLE}/MacOS/dialog-smoke")
    file(MAKE_DIRECTORY "${MM_DIALOG_BUNDLE}/Resources" "${MM_DIALOG_BUNDLE}/MacOS")
    file(WRITE "${MM_DIALOG_BUNDLE}/Info.plist" "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>dialog-smoke</string><key>CFBundleIdentifier</key><string>org.machoman.DialogSmoke</string><key>NSPrincipalClass</key><string>DialogApplication</string><key>NSMainNibFile</key><string>MainMenu</string></dict></plist>")
    add_custom_command(OUTPUT "${MM_DIALOG_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12 -isysroot "${MM_TEST_MACOS_SDK}"
            -fuse-ld=lld -fblocks -framework AppKit -framework CoreServices "${CMAKE_CURRENT_SOURCE_DIR}/guest_dialog_smoke.m" -o "${MM_DIALOG_BINARY}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "../Applications/DialogSmoke.app/Contents/MacOS/dialog-smoke" "${MM_SMOKE_VENV}/bin/dialog-smoke"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_dialog_smoke.m nib_archive_fixture.h nib_lifetime_fixture.h VERBATIM)
    add_custom_target(guest_dialog_smoke_binary ALL DEPENDS "${MM_DIALOG_BINARY}")
    add_test(NAME guest_dialog_smoke COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
        MACHOMAN_STRICT_SELECTORS=1 $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" dialog-smoke)
    set_tests_properties(guest_dialog_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "application principal class, NIBArchive menu, file panels and async cancellation passed" TIMEOUT 20)
endif()

add_executable(test_cocoa_layout test_cocoa_layout.cpp)
add_test(NAME cocoa_layout COMMAND test_cocoa_layout)
if(MM_TEST_MACOS_SDK AND TARGET AppKit)
    set(MM_SCANNING_BINARY "${MM_SMOKE_VENV}/bin/scanning-smoke")
    add_custom_command(OUTPUT "${MM_SCANNING_BINARY}"
        COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12 -isysroot "${MM_TEST_MACOS_SDK}"
            -fuse-ld=lld -fblocks -framework AppKit "${CMAKE_CURRENT_SOURCE_DIR}/guest_scanning_smoke.m" -o "${MM_SCANNING_BINARY}"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_scanning_smoke.m VERBATIM)
    add_custom_target(guest_scanning_smoke_binary ALL DEPENDS "${MM_SCANNING_BINARY}")
    add_test(NAME guest_scanning_smoke COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1
        "MACHOMAN_TEST_PALETTE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rgb-palette.clr"
        MACHOMAN_STRICT_SELECTORS=1 $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" scanning-smoke)
    set_tests_properties(guest_scanning_smoke PROPERTIES TIMEOUT 20)
    if(TARGET evilcocoa)
        add_library(bitmap_drawing_probe SHARED bitmap_drawing_probe.cpp)
        target_include_directories(bitmap_drawing_probe PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/modules/evilcocoa/include")
        target_compile_definitions(bitmap_drawing_probe PRIVATE MM_APPKIT_EVILCOCOA=1)
        set_target_properties(bitmap_drawing_probe PROPERTIES PREFIX "lib" OUTPUT_NAME "bitmap_drawing_probe.dylib" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${MM_SMOKE_VENV}/usr/lib")
        set(MM_BITMAP_BINARY "${MM_SMOKE_VENV}/bin/bitmap-smoke")
        add_custom_command(OUTPUT "${MM_BITMAP_BINARY}"
            COMMAND ${MM_TEST_CLANG} --target=arm64-apple-macos12 -isysroot "${MM_TEST_MACOS_SDK}"
                -fuse-ld=lld -framework AppKit -L "${CMAKE_CURRENT_SOURCE_DIR}" -lbitmap_drawing_probe
                -Wl,-rpath,@executable_path/../usr/lib "${CMAKE_CURRENT_SOURCE_DIR}/guest_bitmap_drawing.m" -o "${MM_BITMAP_BINARY}"
            DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_bitmap_drawing.m libbitmap_drawing_probe.tbd VERBATIM)
        add_custom_target(guest_bitmap_binary ALL DEPENDS "${MM_BITMAP_BINARY}")
        add_dependencies(guest_bitmap_binary bitmap_drawing_probe)
        add_test(NAME guest_bitmap_drawing COMMAND ${CMAKE_COMMAND} -E env MACHOMAN_APPKIT_HEADLESS=1 MACHOMAN_STRICT_SELECTORS=1 $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" bitmap-smoke)
        set_tests_properties(guest_bitmap_drawing PROPERTIES TIMEOUT 20)
    endif()
endif()

if(MM_TEST_MACOS_SDK AND TARGET Foundation)
    set(MM_RPATH_ROOT "${MM_SMOKE_VENV}/rpath-fixture")
    file(MAKE_DIRECTORY "${MM_RPATH_ROOT}/a" "${MM_RPATH_ROOT}/b" "${MM_RPATH_ROOT}/ancestor")
    add_library(rpath_late_host SHARED rpath_late_host.cpp)
    set_target_properties(rpath_late_host PROPERTIES PREFIX "lib" OUTPUT_NAME "local.dylib" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${MM_RPATH_ROOT}")
    set(MM_RPATH_CLANG ${MM_TEST_CLANG} --target=arm64-apple-macos12 -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld)
    add_custom_command(OUTPUT "${MM_RPATH_ROOT}/rpath-loader" "${MM_RPATH_ROOT}/rpath-executable"
        COMMAND ${MM_RPATH_CLANG} -dynamiclib -DFUNCTION=local_value -DVALUE=11
            -Wl,-install_name,@rpath/liblocal.dylib "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_leaf.c" -o "${MM_RPATH_ROOT}/a/liblocal.dylib"
        COMMAND ${MM_RPATH_CLANG} -dynamiclib -DFUNCTION=local_value -DVALUE=22
            -Wl,-install_name,@rpath/liblocal.dylib "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_leaf.c" -o "${MM_RPATH_ROOT}/b/liblocal.dylib"
        COMMAND ${MM_RPATH_CLANG} -dynamiclib -DFUNCTION=inherited_value -DVALUE=33
            -Wl,-install_name,@rpath/libinherited.dylib "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_leaf.c" -o "${MM_RPATH_ROOT}/ancestor/libinherited.dylib"
        COMMAND ${MM_RPATH_CLANG} -dynamiclib -DFUNCTION=exact_value -DVALUE=7
            -Wl,-install_name,@rpath/libexact.dylib "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_leaf.c" -o "${MM_RPATH_ROOT}/libexact.dylib"
        COMMAND ${MM_RPATH_CLANG} -dynamiclib -DFUNCTION=parent_a -DINHERITED=1
            -L "${MM_RPATH_ROOT}/a" -llocal -L "${MM_RPATH_ROOT}/ancestor" -linherited
            -Wl,-install_name,@rpath/libparent_a.dylib -Wl,-rpath,@loader_path
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_parent.c" -o "${MM_RPATH_ROOT}/a/libparent_a.dylib"
        COMMAND ${MM_RPATH_CLANG} -dynamiclib -DFUNCTION=parent_b
            -L "${MM_RPATH_ROOT}/b" -llocal -Wl,-install_name,@rpath/libparent_b.dylib -Wl,-rpath,@loader_path
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_parent.c" -o "${MM_RPATH_ROOT}/b/libparent_b.dylib"
        COMMAND ${MM_RPATH_CLANG} -L "${MM_RPATH_ROOT}/a" -lparent_a -L "${MM_RPATH_ROOT}/b" -lparent_b -L "${MM_RPATH_ROOT}" -lexact
            -Wl,-rpath,@loader_path -Wl,-rpath,@loader_path/a -Wl,-rpath,@loader_path/b -Wl,-rpath,@loader_path/ancestor
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_main.c" -o "${MM_RPATH_ROOT}/rpath-loader"
        COMMAND ${MM_RPATH_CLANG} -L "${MM_RPATH_ROOT}/a" -lparent_a -L "${MM_RPATH_ROOT}/b" -lparent_b -L "${MM_RPATH_ROOT}" -lexact
            -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/a -Wl,-rpath,@executable_path/b -Wl,-rpath,@executable_path/ancestor
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_rpath_main.c" -o "${MM_RPATH_ROOT}/rpath-executable"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_rpath_leaf.c guest_rpath_parent.c guest_rpath_main.c
        BYPRODUCTS "${MM_RPATH_ROOT}/a/liblocal.dylib" "${MM_RPATH_ROOT}/b/liblocal.dylib" "${MM_RPATH_ROOT}/ancestor/libinherited.dylib" "${MM_RPATH_ROOT}/libexact.dylib" "${MM_RPATH_ROOT}/a/libparent_a.dylib" "${MM_RPATH_ROOT}/b/libparent_b.dylib" VERBATIM)
    add_custom_target(guest_rpath_binaries ALL DEPENDS "${MM_RPATH_ROOT}/rpath-loader" "${MM_RPATH_ROOT}/rpath-executable")
    add_dependencies(guest_rpath_binaries rpath_late_host)
    foreach(token IN ITEMS loader executable)
        add_test(NAME guest_rpath_${token} COMMAND $<TARGET_FILE:machoman> run "${MM_SMOKE_VENV}" "${MM_RPATH_ROOT}/rpath-${token}")
        set_tests_properties(guest_rpath_${token} PROPERTIES TIMEOUT 20)
    endforeach()
endif()
if(TARGET machoman_ipc)
    add_executable(test_ipc_kernel test_ipc_kernel.cpp)
    target_link_libraries(test_ipc_kernel PRIVATE machoman_ipc)
    add_test(NAME ipc_kernel COMMAND test_ipc_kernel)
    set_tests_properties(ipc_kernel PROPERTIES TIMEOUT 15)
    add_executable(test_ipc_transport test_ipc_transport.cpp)
    target_link_libraries(test_ipc_transport PRIVATE machoman_ipc)
    add_test(NAME ipc_transport COMMAND test_ipc_transport $<TARGET_FILE:machoman>)
    set_tests_properties(ipc_transport PROPERTIES TIMEOUT 15)
endif()
