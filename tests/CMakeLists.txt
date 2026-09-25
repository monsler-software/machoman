add_executable(test_macho_parse test_macho_parse.cpp)
target_link_libraries(test_macho_parse PRIVATE machoman_core)

add_executable(test_translator test_translator.cpp)
target_link_libraries(test_translator PRIVATE machoman_core)

add_test(NAME macho_parse
         COMMAND test_macho_parse "${CMAKE_SOURCE_DIR}/third_party/samples/rg-arm64")
add_test(NAME translator COMMAND test_translator)

# CommonCrypto lives in the libSystem dylib, so the test links against it.
if(TARGET libSystem)
    add_executable(test_commoncrypto test_commoncrypto.cpp)
    target_link_libraries(test_commoncrypto PRIVATE libSystem)
    add_test(NAME commoncrypto COMMAND test_commoncrypto)
endif()
