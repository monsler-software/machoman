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
