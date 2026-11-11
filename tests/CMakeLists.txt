add_executable(test_macho_parse test_macho_parse.cpp)
target_link_libraries(test_macho_parse PRIVATE machoman_core)

add_executable(test_translator test_translator.cpp)
target_link_libraries(test_translator PRIVATE machoman_core)

add_test(NAME macho_parse
         COMMAND test_macho_parse "${CMAKE_SOURCE_DIR}/third_party/samples/rg-arm64")
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
if(MM_TEST_MACOS_SDK AND TARGET Foundation)
    find_program(MM_TEST_CLANG clang REQUIRED)
    set(MM_SMOKE_VENV "${CMAKE_CURRENT_BINARY_DIR}/guest-venv")
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
            -fuse-ld=lld -fblocks -framework AppKit "${CMAKE_CURRENT_SOURCE_DIR}/guest_dialog_smoke.m" -o "${MM_DIALOG_BINARY}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "../Applications/DialogSmoke.app/Contents/MacOS/dialog-smoke" "${MM_SMOKE_VENV}/bin/dialog-smoke"
        DEPENDS "${MM_SMOKE_VENV}/machoman-venv.json" guest_dialog_smoke.m nib_archive_fixture.h VERBATIM)
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
