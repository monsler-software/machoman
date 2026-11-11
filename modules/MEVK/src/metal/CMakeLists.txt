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
target_link_libraries(MEVK_CoreGraphics PRIVATE MEVK_Metal libobjc)
target_compile_options(MEVK_CoreGraphics PRIVATE -Wall -Wextra -fvisibility=hidden)
set(MEVK_COREGRAPHICS_DIR "${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks/CoreGraphics.framework/Versions/A")
if(IS_SYMLINK "${MEVK_COREGRAPHICS_DIR}/CoreGraphics.so")
    file(REMOVE "${MEVK_COREGRAPHICS_DIR}/CoreGraphics.so")
endif()
set_target_properties(MEVK_CoreGraphics PROPERTIES
    PREFIX "" OUTPUT_NAME "CoreGraphics" SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${MEVK_COREGRAPHICS_DIR}"
    BUILD_RPATH "${MEVK_METAL_DIR};${CMAKE_BINARY_DIR}/dylibs/usr/lib"
    INSTALL_RPATH "${MEVK_METAL_DIR};${CMAKE_BINARY_DIR}/dylibs/usr/lib")

if(MEVK_BUILD_TESTS)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../tests/metal" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
