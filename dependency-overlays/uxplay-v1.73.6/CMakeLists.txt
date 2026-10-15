cmake_minimum_required(VERSION 3.16)
project(uxplay-airplay C)

set(DEPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Prepared dependency root")
set(UXPLAY_LIB "${DEPS_ROOT}/UxPlay/lib")
set(LIBPLIST_DIR "${DEPS_ROOT}/libplist-2.7.0")
set(W32_PTHREADS_DIR
    "${DEPS_ROOT}/obs-sdk/obs-studio-30.0.0/deps/w32-pthreads")

if(NOT DEFINED OPENSSL_DIR OR OPENSSL_DIR STREQUAL "")
    message(FATAL_ERROR "OPENSSL_DIR must point to the prepared local OpenSSL dependency")
endif()

aux_source_directory(${UXPLAY_LIB}/playfair playfair_src)
add_library(playfair STATIC ${playfair_src})
target_include_directories(playfair PUBLIC ${UXPLAY_LIB}/playfair)
target_compile_definitions(playfair PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)
if(MSVC)
    target_compile_options(playfair PRIVATE /wd4244 /wd4267 /wd4996)
endif()

aux_source_directory(${UXPLAY_LIB}/llhttp llhttp_src)
add_library(llhttp STATIC ${llhttp_src})
target_include_directories(llhttp PUBLIC ${UXPLAY_LIB}/llhttp)
target_compile_definitions(llhttp PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(llhttp PRIVATE /wd4244 /wd4267 /wd4996)
endif()

set(AIRPLAY_SOURCES
    ${UXPLAY_LIB}/airplay_video.c
    ${UXPLAY_LIB}/byteutils.c
    ${UXPLAY_LIB}/compat.c
    ${UXPLAY_LIB}/crypto.c
    ${UXPLAY_LIB}/dnssd.c
    ${UXPLAY_LIB}/fairplay_playfair.c
    ${UXPLAY_LIB}/http_request.c
    ${UXPLAY_LIB}/http_response.c
    ${UXPLAY_LIB}/httpd.c
    ${UXPLAY_LIB}/logger.c
    ${UXPLAY_LIB}/mirror_buffer.c
    ${UXPLAY_LIB}/netutils.c
    ${UXPLAY_LIB}/pairing.c
    ${UXPLAY_LIB}/raop.c
    ${UXPLAY_LIB}/raop_buffer.c
    ${UXPLAY_LIB}/raop_ntp.c
    ${UXPLAY_LIB}/raop_rtp.c
    ${UXPLAY_LIB}/raop_rtp_mirror.c
    ${UXPLAY_LIB}/srp.c
    ${UXPLAY_LIB}/utils.c
)

add_library(airplay STATIC ${AIRPLAY_SOURCES})
target_include_directories(airplay PUBLIC
    ${UXPLAY_LIB}
    ${UXPLAY_LIB}/playfair
    ${UXPLAY_LIB}/llhttp
    ${LIBPLIST_DIR}/include
    ${OPENSSL_DIR}/include
    ${W32_PTHREADS_DIR}
)
target_compile_definitions(airplay PUBLIC
    WIN32
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    WIN32_LEAN_AND_MEAN
    LIBPLIST_STATIC
    OPENSSL_API_COMPAT=0x10101000L
    PLIST_210
    PLIST_230
)
if(MSVC)
    target_compile_options(airplay PRIVATE
        /wd4244 /wd4267 /wd4996 /wd4018 /wd4146 /wd4005 /wd4133
    )
endif()

unset(OPENSSL_CRYPTO_LIB CACHE)
find_library(OPENSSL_CRYPTO_LIB NAMES libcrypto crypto libcrypto-3-x64
    PATHS
        "${OPENSSL_DIR}/lib"
        "${OPENSSL_DIR}/lib/VC"
        "${OPENSSL_DIR}/lib/VC/x64"
        "${OPENSSL_DIR}/lib/VC/x64/MD"
        "${OPENSSL_DIR}/lib/VC/x64/MT"
        "${OPENSSL_DIR}/lib/VC/x86_64"
    NO_DEFAULT_PATH
)
if(NOT OPENSSL_CRYPTO_LIB)
    message(FATAL_ERROR "OpenSSL import library not found under OPENSSL_DIR")
endif()

target_link_libraries(airplay PUBLIC
    playfair
    llhttp
    ${LIBPLIST_DIR}/build/plist.lib
    ${OPENSSL_CRYPTO_LIB}
    ${DEPS_ROOT}/w32-pthreads.lib
    ws2_32
    iphlpapi
)
