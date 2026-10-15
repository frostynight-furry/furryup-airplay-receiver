cmake_minimum_required(VERSION 3.16)
project(airplay-live-source VERSION 0.1.0 LANGUAGES C)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "AirPlay Live Source currently requires the x64 MSVC toolchain")
endif()

# --- Paths ---
set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps")

# UxPlay built libs
set(UXPLAY_BUILD_DIR "${DEPS_DIR}/uxplay-build/build")
if(DEFINED FUS_UXPLAY_BUILD_DIR)
    set(UXPLAY_BUILD_DIR "${FUS_UXPLAY_BUILD_DIR}")
endif()
set(UXPLAY_LIB_DIR "${DEPS_DIR}/UxPlay/lib")

# OBS headers. Build against the oldest supported release so OBS 32+'s module
# version gate also accepts this binary. Newer hosts remain API compatible.
set(OBS_SDK_VERSION "30.0.0" CACHE STRING "Oldest supported OBS SDK")
set(OBS_SRC_DIR "${DEPS_DIR}/obs-sdk/obs-studio-${OBS_SDK_VERSION}"
    CACHE PATH "OBS Studio source/SDK root")

# Pinned FFmpeg 8.1 development SDK. The internal alpha intentionally uses the
# FFmpeg 8.1 runtime already loaded from its OBS 32.2.2 host. Shipping another
# same-named DLL set beside a plugin does not isolate it on Windows.
set(FFMPEG_ROOT "${DEPS_DIR}/ffmpeg-sdk"
    CACHE PATH "FFmpeg 8.1 shared development SDK root")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

# OpenSSL
if(NOT DEFINED OPENSSL_DIR OR OPENSSL_DIR STREQUAL "")
    if(DEFINED ENV{OPENSSL_ROOT_DIR})
        set(OPENSSL_DIR "$ENV{OPENSSL_ROOT_DIR}")
    elseif(EXISTS "C:/Program Files/OpenSSL-Win64")
        set(OPENSSL_DIR "C:/Program Files/OpenSSL-Win64")
    elseif(EXISTS "C:/Program Files/OpenSSL")
        set(OPENSSL_DIR "C:/Program Files/OpenSSL")
    else()
        message(FATAL_ERROR
            "OpenSSL 3 x64 was not found; set OPENSSL_DIR explicitly")
    endif()
endif()
set(OPENSSL_DIR "${OPENSSL_DIR}" CACHE PATH "OpenSSL installation root")

# Find OpenSSL libcrypto
unset(OPENSSL_CRYPTO_LIB CACHE)
find_library(OPENSSL_CRYPTO_LIB NAMES libcrypto crypto libcrypto-3-x64
    PATHS
        "${OPENSSL_DIR}/lib"
        "${OPENSSL_DIR}/lib/VC"
        "${OPENSSL_DIR}/lib/VC/x64"
        "${OPENSSL_DIR}/lib/VC/x64/MD"
        "${OPENSSL_DIR}/lib/VC/x64/MT"
        "${OPENSSL_DIR}/lib/VC/x86_64"
    NO_DEFAULT_PATH)
message(STATUS "OpenSSL crypto lib: ${OPENSSL_CRYPTO_LIB}")

foreach(REQUIRED_DIR
        "${OBS_SRC_DIR}/libobs"
        "${OBS_SRC_DIR}/deps/w32-pthreads"
        "${FFMPEG_INCLUDE_DIR}"
        "${FFMPEG_LIB_DIR}")
    if(NOT IS_DIRECTORY "${REQUIRED_DIR}")
        message(FATAL_ERROR "Required dependency directory not found: ${REQUIRED_DIR}")
    endif()
endforeach()

if(NOT OPENSSL_CRYPTO_LIB)
    message(FATAL_ERROR "OpenSSL libcrypto import library was not found")
endif()

file(GLOB OPENSSL_RUNTIME_FILES
    "${OPENSSL_DIR}/bin/libcrypto-3*-x64.dll"
    "${OPENSSL_DIR}/libcrypto-3*-x64.dll")
if(NOT OPENSSL_RUNTIME_FILES)
    message(FATAL_ERROR
        "OpenSSL runtime DLL was not found under ${OPENSSL_DIR}; a shared OpenSSL 3 x64 package is required")
endif()

# libplist
set(LIBPLIST_DIR "${DEPS_DIR}/libplist-2.7.0")

# --- Plugin shared library ---
add_library(airplay-live-source MODULE
    src/plugin-main.c
    src/frontend-controller.c
    src/airplay-source.c
    src/status-frame.c
    src/video-decoder.c
    src/audio-decoder.c
)

# --- Include directories ---
target_include_directories(airplay-live-source PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    # OBS headers
    ${OBS_SRC_DIR}/libobs
    ${OBS_SRC_DIR}/deps/w32-pthreads
    # UxPlay headers (raop.h, dnssd.h, stream.h, logger.h)
    ${UXPLAY_LIB_DIR}
    ${UXPLAY_LIB_DIR}/playfair
    ${UXPLAY_LIB_DIR}/llhttp
    # libplist headers
    ${LIBPLIST_DIR}/include
    # FFmpeg headers
    ${FFMPEG_INCLUDE_DIR}
    # OpenSSL headers
    ${OPENSSL_DIR}/include
)

# --- Link libraries ---
# UxPlay static libs
target_link_libraries(airplay-live-source PRIVATE
    ${UXPLAY_BUILD_DIR}/airplay.lib
    ${UXPLAY_BUILD_DIR}/playfair.lib
    ${UXPLAY_BUILD_DIR}/llhttp.lib
)

# libplist
target_link_libraries(airplay-live-source PRIVATE
    ${LIBPLIST_DIR}/build/plist.lib
)

# OBS
target_link_libraries(airplay-live-source PRIVATE ${DEPS_DIR}/obs.lib)

# OpenSSL
target_link_libraries(airplay-live-source PRIVATE
    ${OPENSSL_CRYPTO_LIB}
)

# FFmpeg
target_link_directories(airplay-live-source PRIVATE ${FFMPEG_LIB_DIR})
target_link_libraries(airplay-live-source PRIVATE
    avcodec avutil swscale swresample
)

# Windows (dnssd.dll is loaded dynamically by UxPlay's dnssd.c at runtime)
target_link_libraries(airplay-live-source PRIVATE
    ws2_32 iphlpapi user32 gdi32
    ${DEPS_DIR}/w32-pthreads.lib
)

target_compile_definitions(airplay-live-source PRIVATE
    WIN32
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    LIBPLIST_STATIC
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

set_target_properties(airplay-live-source PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    PREFIX ""
)

# Deliberately no `cmake --install` deployment target. A safe installation
# also needs host-version checks, upstream-plugin conflict checks, locale and
# notice staging, legacy FFmpeg cleanup, and firewall handling. Those steps
# are performed only by the complete internal-alpha artifact/installer.
