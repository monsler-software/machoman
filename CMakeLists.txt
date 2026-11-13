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

    add_library(libobjc SHARED src/dylibs/libobjc/objc.cpp)
    target_link_libraries(libobjc PRIVATE machoman_core)
    target_compile_options(libobjc PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(libobjc PROPERTIES
        PREFIX "" OUTPUT_NAME "libobjc.A.dylib" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/usr/lib")

    # os_log handles participate in Objective-C reference counting.
    target_link_libraries(libSystem PRIVATE libobjc)

    add_subdirectory(src/dylibs/swiftCore)

    # CoreFoundation + Foundation: one object, since the types are the same
    # (toll-free bridging) and both frameworks' install names point at it.
    add_library(Foundation SHARED
        src/dylibs/Foundation/strings.cpp src/dylibs/Foundation/collections.cpp src/dylibs/Foundation/index_set.cpp
        src/dylibs/Foundation/plists.cpp src/dylibs/Foundation/misc.cpp src/dylibs/Foundation/task.cpp src/dylibs/Foundation/dispatch.cpp
        src/dylibs/Foundation/dispatch_data.cpp
        src/dylibs/Foundation/cookies.cpp
        src/dylibs/Foundation/url_request.cpp
        src/dylibs/Foundation/key_value.cpp src/dylibs/Foundation/threading.cpp
        src/dylibs/Foundation/geometry.cpp
        src/dylibs/Foundation/formatting.cpp
        src/dylibs/Foundation/json.cpp
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
    # New SDKs bind NSURLRequest through CFNetwork's re-export.
    add_custom_command(TARGET Foundation POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CFNetwork.framework/Versions/A"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "../../../CoreFoundation.framework/Versions/A/CoreFoundation.so"
            "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork.so")

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
        src/dylibs/AppKit/webview.cpp
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
                COMMAND ${MM_WAYLAND_SCANNER} private-code "${MM_ICON_XML}" "${MM_XDG_DIR}/xdg-toplevel-icon-v1-protocol.c"
                DEPENDS "${MM_ICON_XML}" VERBATIM)
            list(APPEND MM_APPKIT_SOURCES "${MM_XDG_DIR}/xdg-toplevel-icon-v1-client-protocol.h" "${MM_XDG_DIR}/xdg-toplevel-icon-v1-protocol.c")
        endif()
    else()
        message(STATUS "Wayland (wayland-client, wayland-egl, egl, xkbcommon, wayland-protocols) not found: AppKit runs without a display")
        list(APPEND MM_APPKIT_SOURCES src/dylibs/AppKit/platform_headless.cpp)
    endif()
    list(APPEND MM_APPKIT_SOURCES src/dylibs/symbol-synthesis/synthesise.cpp)
    add_library(AppKit SHARED ${MM_APPKIT_SOURCES})
    if(MM_WAYLAND_FOUND AND EXISTS "${MM_DATA_CONTROL_XML}")
        target_compile_definitions(AppKit PRIVATE MM_APPKIT_DATA_CONTROL=1)
    endif()
    if(MM_GIO_FOUND)
        target_link_libraries(AppKit PRIVATE PkgConfig::MM_GIO)
        target_compile_definitions(AppKit PRIVATE MM_APPKIT_GIO=1)
    endif()
    if(MM_GTK_FOUND)
        target_link_libraries(AppKit PRIVATE PkgConfig::MM_GTK)
        target_compile_definitions(AppKit PRIVATE MM_APPKIT_GTK=1)
    endif()
    if(MM_PIXBUF_FOUND)
        target_link_libraries(AppKit PRIVATE PkgConfig::MM_PIXBUF)
        target_compile_definitions(AppKit PRIVATE MM_APPKIT_PIXBUF=1)
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

    add_subdirectory(modules/WebKit)

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


    option(MACHOMAN_PIPEWIRE "Enable PipeWire audio output when available" ON)
    if(MACHOMAN_PIPEWIRE AND PkgConfig_FOUND)
        pkg_check_modules(MM_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3>=0.3.50)
    endif()
    add_library(machoman_audio_pipewire STATIC src/dylibs/audio/backend_pipewire.cpp)
    target_link_libraries(machoman_audio_pipewire PUBLIC machoman_audio_engine)
    target_compile_options(machoman_audio_pipewire PRIVATE -Wall -Wextra -Wconversion)
    set_target_properties(machoman_audio_pipewire PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MACHOMAN_PIPEWIRE AND MM_PIPEWIRE_FOUND)
        target_link_libraries(machoman_audio_pipewire PRIVATE PkgConfig::MM_PIPEWIRE)
        target_compile_definitions(machoman_audio_pipewire PRIVATE MM_AUDIO_PIPEWIRE=1)
    endif()

    add_library(audio SHARED src/dylibs/audio/coremidi.cpp src/dylibs/audio/coreaudio_hal.cpp
        src/dylibs/audio/audiounit.cpp src/dylibs/audio/output_session.cpp
        src/dylibs/audio/property_listeners.cpp src/dylibs/audio/notification_dispatch.cpp
        src/dylibs/symbol-synthesis/synthesise.cpp)
    target_link_libraries(audio PRIVATE machoman_core libobjc Foundation machoman_audio_pipewire)
    target_include_directories(audio PRIVATE src/dylibs/symbol-synthesis)
    target_compile_options(audio PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(audio PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A"
        INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib;${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreFoundation.framework/Versions/A"
        PREFIX "" OUTPUT_NAME "CoreMIDI" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreMIDI.framework/Versions/A")
    foreach(fw CoreAudio AudioToolbox AudioUnit CoreAudioKit)
        add_custom_command(TARGET audio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/${fw}.framework/Versions/A"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../CoreMIDI.framework/Versions/A/CoreMIDI.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/${fw}.framework/Versions/A/${fw}.so")
    endforeach()

    # Carbon: the keyboard/mouse/event-loop leftovers a Cocoa program still
    # calls. Also stands in for ApplicationServices and CoreServices, which
    # re-export it and otherwise reach libSystem through the linker.
    add_library(carbon SHARED src/dylibs/carbon/carbon.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
    target_link_libraries(carbon PRIVATE machoman_core libobjc Foundation)
    target_include_directories(carbon PRIVATE src/dylibs/symbol-synthesis)
    target_compile_options(carbon PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(carbon PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib" INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
        PREFIX "" OUTPUT_NAME "Carbon" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Carbon.framework/Versions/A")
    foreach(fw ApplicationServices CoreServices)
        add_custom_command(TARGET carbon POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/${fw}.framework/Versions/A"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "../../../Carbon.framework/Versions/A/Carbon.so"
                "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/${fw}.framework/Versions/A/${fw}.so")
    endforeach()

    # Accelerate: vDSP's vector arithmetic and FFT, implemented rather than
    # stubbed — a spectrum display that gets zeros back is worse than a
    # slower one.
    add_library(accelerate SHARED src/dylibs/accelerate/vdsp.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
    target_link_libraries(accelerate PRIVATE machoman_core libobjc)
    target_include_directories(accelerate PRIVATE src/dylibs/symbol-synthesis)
    target_compile_options(accelerate PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(accelerate PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib" INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
        PREFIX "" OUTPUT_NAME "Accelerate" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/Accelerate.framework/Versions/A")

    # CoreVideo: the display link a program draws to (src/dylibs/audio).
    add_library(CoreVideo SHARED src/dylibs/audio/displaylink.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
    target_link_libraries(CoreVideo PRIVATE machoman_core libobjc)
    target_include_directories(CoreVideo PRIVATE src/dylibs/symbol-synthesis)
    target_compile_options(CoreVideo PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(CoreVideo PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib" INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
        PREFIX "" OUTPUT_NAME "CoreVideo" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreVideo.framework/Versions/A")

    # CoreText supplies font descriptors backed by the host's installed fonts.
    find_package(Fontconfig REQUIRED)
    add_library(CoreText SHARED src/dylibs/CoreText/fonts.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
    target_include_directories(CoreText PRIVATE src/dylibs/symbol-synthesis)
    target_link_libraries(CoreText PRIVATE machoman_core libobjc Foundation Fontconfig::Fontconfig)
    target_compile_options(CoreText PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(CoreText PROPERTIES
        PREFIX "" OUTPUT_NAME "CoreText" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreText.framework/Versions/A")

    # Stub frameworks: one object synthesising symbols from their names
    # (src/dylibs/stub-framework), installed under every framework machoman
    # does not implement yet. Adding a name here is all a new stub needs.
    add_library(stub_framework SHARED src/dylibs/symbol-synthesis/stub_framework.cpp src/dylibs/symbol-synthesis/synthesise.cpp)
    target_link_libraries(stub_framework PRIVATE machoman_core libobjc)
    target_include_directories(stub_framework PRIVATE src/dylibs/symbol-synthesis)
    target_compile_options(stub_framework PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(stub_framework PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib" INSTALL_RPATH "${CMAKE_BINARY_DIR}/dylibs/usr/lib"
        PREFIX "" OUTPUT_NAME "machoman-stub-framework" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/usr/lib")
    set(MM_STUB_FRAMEWORKS
        AVFoundation:A CoreMedia:A VideoToolbox:A
        CoreGraphics:A Metal:A MetalKit:A GameController:A WebKit:A
        Security:A SystemConfiguration:A DiskArbitration:A IOKit:A
        UniformTypeIdentifiers:A ImageIO:A CoreML:A UserNotifications:A
        ServiceManagement:A CoreHaptics:A ForceFeedback:A IOSurface:A Contacts:A)
    if(TARGET MEVK_Metal)
        list(REMOVE_ITEM MM_STUB_FRAMEWORKS Metal:A CoreGraphics:A)
    endif()
    if(TARGET WebKit)
        list(REMOVE_ITEM MM_STUB_FRAMEWORKS WebKit:A)
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
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "machoman-stub-framework.so" "${CMAKE_BINARY_DIR}/dylibs/usr/lib/${lib}.so")
    endforeach()

    # libiconv: glibc's iconv under Apple's two sets of names.
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
