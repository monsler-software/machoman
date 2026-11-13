add_library(swiftCore SHARED heap.cpp synchronization.cpp)
target_include_directories(swiftCore PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(swiftCore PRIVATE Threads::Threads)
target_compile_options(swiftCore PRIVATE -Wall -Wextra -Wconversion -fvisibility=hidden)
set_target_properties(swiftCore PROPERTIES
    PREFIX "" OUTPUT_NAME "libswiftCore.dylib" SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/usr/lib/swift")
