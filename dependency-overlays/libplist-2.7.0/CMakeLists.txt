cmake_minimum_required(VERSION 3.16)
project(libplist C CXX)

add_library(plist STATIC
    src/base64.c
    src/bplist.c
    src/bytearray.c
    src/hashtable.c
    src/jplist.c
    src/jsmn.c
    src/oplist.c
    src/out-default.c
    src/out-limd.c
    src/out-plutil.c
    src/plist.c
    src/ptrarray.c
    src/time64.c
    src/xplist.c
    libcnary/cnary.c
    libcnary/node.c
    libcnary/node_list.c
)

target_include_directories(plist PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libcnary/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(plist PRIVATE
    HAVE_CONFIG_H=1
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
)

if(MSVC)
    target_compile_options(plist PRIVATE
        /wd4244 /wd4267 /wd4996 /wd4018 /wd4047 /wd4024 /wd4789
    )
endif()
