# Guest tests live with MEVK. No changes to machoman's test suite are needed.
set(MEVK_TEST_MACOS_SDK "" CACHE PATH "macOS SDK for the MEVK arm64 guest test")
if(MEVK_TEST_MACOS_SDK)
    find_program(MEVK_TEST_CLANG clang REQUIRED)
    set(MEVK_GUEST_VENV "${CMAKE_CURRENT_BINARY_DIR}/guest-venv")
    set(MEVK_GUEST_BINARY "${MEVK_GUEST_VENV}/bin/mevk-metal-smoke")
    add_custom_command(OUTPUT "${MEVK_GUEST_VENV}/machoman-venv.json"
        COMMAND ${CMAKE_COMMAND} -DMACHOMAN=$<TARGET_FILE:machoman>
            -DVENV=${MEVK_GUEST_VENV} -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare_venv.cmake"
        DEPENDS machoman prepare_venv.cmake VERBATIM)
    add_custom_command(OUTPUT "${MEVK_GUEST_BINARY}"
        COMMAND ${MEVK_TEST_CLANG} --target=arm64-apple-macos12
            -isysroot "${MEVK_TEST_MACOS_SDK}" -fuse-ld=lld -fno-objc-arc -fblocks
            -framework Foundation -framework Metal -framework CoreGraphics
            "${CMAKE_CURRENT_SOURCE_DIR}/guest_metal_smoke.m" -o "${MEVK_GUEST_BINARY}"
        DEPENDS "${MEVK_GUEST_VENV}/machoman-venv.json" guest_metal_smoke.m VERBATIM)
    add_custom_target(mevk_guest_metal_smoke ALL DEPENDS "${MEVK_GUEST_BINARY}")
    add_dependencies(mevk_guest_metal_smoke MEVK_Metal MEVK_CoreGraphics libSystem)
    add_test(NAME mevk_guest_metal
        COMMAND ${CMAKE_COMMAND} -E env
            MACHOMAN_MEVK_TRACE=1 MACHOMAN_STRICT_SELECTORS=1
            DYLD_FRAMEWORK_PATH=${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks
            $<TARGET_FILE:machoman> run "${MEVK_GUEST_VENV}" mevk-metal-smoke)
    set_tests_properties(mevk_guest_metal PROPERTIES
        PASS_REGULAR_EXPRESSION "MEVK Metal guest smoke passed"
        TIMEOUT 30)
    if(MEVK_TEST_METALLIB AND EXISTS "${MEVK_TEST_METALLIB}")
        set(MEVK_RENDER_GUEST_BINARY "${MEVK_GUEST_VENV}/bin/mevk-metal-render")
        add_custom_command(OUTPUT "${MEVK_RENDER_GUEST_BINARY}"
            COMMAND ${MEVK_TEST_CLANG} --target=arm64-apple-macos12
                -isysroot "${MEVK_TEST_MACOS_SDK}" -fuse-ld=lld -fno-objc-arc -fblocks
                -framework Foundation -framework AppKit -framework QuartzCore -framework Metal -framework CoreVideo
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_metal_render.m" -o "${MEVK_RENDER_GUEST_BINARY}"
            DEPENDS "${MEVK_GUEST_VENV}/machoman-venv.json" guest_metal_render.m VERBATIM)
        add_custom_target(mevk_guest_metal_render ALL DEPENDS "${MEVK_RENDER_GUEST_BINARY}")
        add_dependencies(mevk_guest_metal_render MEVK_Metal AppKit libSystem)
        add_test(NAME mevk_guest_render
            COMMAND ${CMAKE_COMMAND} -E env
                MACHOMAN_MEVK_TRACE=1 MACHOMAN_STRICT_SELECTORS=1 MACHOMAN_APPKIT_HEADLESS=1 MACHOMAN_DISPLAY_HZ=165.002
                DYLD_FRAMEWORK_PATH=${CMAKE_BINARY_DIR}/dylibs/System/Library/Frameworks
                $<TARGET_FILE:machoman> run "${MEVK_GUEST_VENV}" mevk-metal-render "${MEVK_TEST_METALLIB}")
        set_tests_properties(mevk_guest_render PROPERTIES
            PASS_REGULAR_EXPRESSION "MEVK Metal guest render and AppKit drawable passed" TIMEOUT 60)
    endif()
endif()
