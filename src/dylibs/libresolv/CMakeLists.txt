# Build the portable DNS wire-format routines from the supplied Apple
# libresolv checkout. Resolver configuration belongs to the Linux host.
set(MM_LIBRESOLV_SOURCE "${CMAKE_SOURCE_DIR}/modules/libresolv" CACHE PATH "Apple libresolv source checkout")
if(NOT EXISTS "${MM_LIBRESOLV_SOURCE}/ns_name.c")
    message(FATAL_ERROR "libresolv source missing: clone https://github.com/monsler-software/machoman-libresolv into modules/libresolv")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/arpa")
configure_file("${MM_LIBRESOLV_SOURCE}/nameser.h" "${CMAKE_CURRENT_BINARY_DIR}/include/arpa/nameser.h" COPYONLY)
add_library(libresolv SHARED libresolv.cpp
    "${MM_LIBRESOLV_SOURCE}/ns_name.c" "${MM_LIBRESOLV_SOURCE}/ns_netint.c"
    "${MM_LIBRESOLV_SOURCE}/ns_parse.c" "${MM_LIBRESOLV_SOURCE}/res_comp.c"
    "${MM_LIBRESOLV_SOURCE}/base64.c")
target_include_directories(libresolv PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<$<COMPILE_LANGUAGE:C>:${MM_LIBRESOLV_SOURCE}>")
set_source_files_properties(
    "${MM_LIBRESOLV_SOURCE}/ns_name.c" "${MM_LIBRESOLV_SOURCE}/ns_netint.c"
    "${MM_LIBRESOLV_SOURCE}/ns_parse.c" "${MM_LIBRESOLV_SOURCE}/res_comp.c"
    "${MM_LIBRESOLV_SOURCE}/base64.c" PROPERTIES COMPILE_DEFINITIONS "__APPLE__;USE__RES_9;__private_extern__=extern")
target_link_libraries(libresolv PRIVATE resolv)
set_target_properties(libresolv PROPERTIES PREFIX "" OUTPUT_NAME "libresolv.9.dylib" SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dylibs/usr/lib")
