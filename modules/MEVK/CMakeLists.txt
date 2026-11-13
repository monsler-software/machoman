cmake_minimum_required(VERSION 3.25)

# This directory can also be configured without the rest of machoman.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(MEVK VERSION 0.1.0 LANGUAGES C CXX)
endif()

find_package(Vulkan REQUIRED)

find_package(Threads REQUIRED)
find_package(LLVM CONFIG REQUIRED)
if(LLVM_PACKAGE_VERSION VERSION_LESS "23.0")
    message(FATAL_ERROR "MEVK AIR translation requires LLVM 23 or newer")
endif()
find_path(MEVK_SHADERC_INCLUDE shaderc/shaderc.hpp REQUIRED)
find_library(MEVK_SHADERC_LIBRARY shaderc_shared REQUIRED)

# Invalidate persisted translations when the emitter, reflection/cache schema,
# LLVM reader or shaderc compiler changes. Edits trigger CMake regeneration.
set(MEVK_CACHE_SOURCES src/shader/air.cpp src/shader/cache.cpp src/shader/cache.h include/mevk/shader.h)
set(MEVK_CACHE_SIGNATURE "${LLVM_PACKAGE_VERSION}")
foreach(source IN LISTS MEVK_CACHE_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
    string(APPEND MEVK_CACHE_SIGNATURE "${source_hash}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MEVK_SHADERC_LIBRARY}")
file(SHA256 "${MEVK_SHADERC_LIBRARY}" shaderc_hash)
string(SHA256 MEVK_CACHE_REVISION "${MEVK_CACHE_SIGNATURE}${shaderc_hash}")

add_library(mevk STATIC
    src/vulkan/context.cpp
    src/vulkan/buffer.cpp
    src/vulkan/command_buffer.cpp
    src/vulkan/render.cpp
    src/vulkan/compute.cpp
    src/vulkan/texture.cpp
    src/vulkan/present.cpp
    src/shader/air.cpp
    src/shader/cache.cpp
    src/shader/metallib.cpp)
add_library(MEVK::mevk ALIAS mevk)
target_compile_features(mevk PUBLIC cxx_std_26)
target_compile_definitions(mevk PRIVATE MEVK_SHADER_CACHE_REVISION="${MEVK_CACHE_REVISION}")
set_target_properties(mevk PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(mevk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(mevk SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${MEVK_SHADERC_INCLUDE})
target_link_libraries(mevk PRIVATE Vulkan::Vulkan Threads::Threads LLVM ${MEVK_SHADERC_LIBRARY})
if(MSVC)
    target_compile_options(mevk PRIVATE /W4)
else()
    target_compile_options(mevk PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
endif()

option(MEVK_BUILD_TESTS "Build MEVK GPU integration tests (requires a Vulkan device)" OFF)
if(MEVK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(MEVK_BUILD_TOOLS "Build the MEVK Vulkan device probe" ON)
if(MEVK_BUILD_TOOLS)
    add_executable(mevk_probe tools/mevk_probe.cpp)
    set_target_properties(mevk_probe PROPERTIES OUTPUT_NAME "mevk-probe")
    target_link_libraries(mevk_probe PRIVATE MEVK::mevk)
    add_executable(mevk_shader_inspect tools/mevk_shader_inspect.cpp)
    set_target_properties(mevk_shader_inspect PROPERTIES OUTPUT_NAME "mevk-shader-inspect")
    target_link_libraries(mevk_shader_inspect PRIVATE MEVK::mevk)
endif()
