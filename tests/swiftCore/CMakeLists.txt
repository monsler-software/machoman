add_executable(test_swift_runtime test_swift_runtime.cpp)
target_include_directories(test_swift_runtime PRIVATE "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/dylibs/swiftCore")
target_link_libraries(test_swift_runtime PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(test_swift_runtime PRIVATE -Wall -Wextra -Wconversion)
add_dependencies(test_swift_runtime swiftCore)
add_test(NAME swift_runtime COMMAND test_swift_runtime $<TARGET_FILE:swiftCore>)
set_tests_properties(swift_runtime PROPERTIES TIMEOUT 20)

# C with Clang's swiftcall/swift_context attributes exercises Swift's actual
# destructor ABI without requiring a Swift compiler or a downloaded toolchain.
if(MM_TEST_MACOS_SDK)
    find_program(MM_SWIFT_TEST_CLANG clang REQUIRED)
    set(MM_SWIFT_VENV "${CMAKE_CURRENT_BINARY_DIR}/guest-venv")
    add_custom_command(OUTPUT "${MM_SWIFT_VENV}/machoman-venv.json"
        COMMAND ${CMAKE_COMMAND} -DMACHOMAN=$<TARGET_FILE:machoman> -DVENV=${MM_SWIFT_VENV}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_guest_venv.cmake"
        DEPENDS machoman "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_guest_venv.cmake" VERBATIM)
    foreach(MM_SWIFT_ARCH IN ITEMS arm64 x86_64)
        set(MM_SWIFT_BINARY "${MM_SWIFT_VENV}/bin/swift-runtime-${MM_SWIFT_ARCH}")
        add_custom_command(OUTPUT "${MM_SWIFT_BINARY}"
            COMMAND ${MM_SWIFT_TEST_CLANG} --target=${MM_SWIFT_ARCH}-apple-macos12
                -isysroot "${MM_TEST_MACOS_SDK}" -fuse-ld=lld -O2
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_swift_runtime.c"
                -L "${MM_TEST_MACOS_SDK}/usr/lib/swift" -lswiftCore -o "${MM_SWIFT_BINARY}"
            DEPENDS "${MM_SWIFT_VENV}/machoman-venv.json" guest_swift_runtime.c VERBATIM)
        add_custom_target(guest_swift_runtime_${MM_SWIFT_ARCH}_binary ALL DEPENDS "${MM_SWIFT_BINARY}")
        add_dependencies(guest_swift_runtime_${MM_SWIFT_ARCH}_binary swiftCore)
        add_test(NAME guest_swift_runtime_${MM_SWIFT_ARCH}
            COMMAND $<TARGET_FILE:machoman> run "${MM_SWIFT_VENV}" swift-runtime-${MM_SWIFT_ARCH})
        set_tests_properties(guest_swift_runtime_${MM_SWIFT_ARCH} PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "Swift heap ABI, guest swiftself destructors, once and exclusivity passed")
    endforeach()
endif()
