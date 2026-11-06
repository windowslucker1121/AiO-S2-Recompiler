# SPDX-License-Identifier: GPL-3.0-or-later
# libvpx (BSD-3-Clause; x86inc macros ISC) for the host nn::codec VP9/VP8 decoders (src/media/video_codec.cpp).
# libvpx builds with its own configure script, not CMake, so it is taken prebuilt from vcpkg, static with the static MSVC runtime (/MT)
# like everything else here:
#     vcpkg install libvpx:x64-windows-static        (vcpkg port libvpx 1.16.0#3)
# NXRT_LIBVPX_ROOT points at that install (default: C:/vcpkg, then $VCPKG_ROOT) (the folder with include/vpx and lib/vpx.lib). Without it the runtime builds without the
# decoders (NXRT_HAVE_LIBVPX=0) and Switch 2 movies are left out.
set(NXRT_LIBVPX_ROOT "" CACHE PATH "vcpkg x64-windows-static install with libvpx (include/vpx, lib/vpx.lib); empty: look in the usual places")
set(_nxrt_vpx_roots "${NXRT_LIBVPX_ROOT}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/vcpkg/installed/x64-windows-static"   # repo-local vcpkg (git-ignored)
    "C:/vcpkg/installed/x64-windows-static")
if(DEFINED ENV{VCPKG_ROOT})
    list(APPEND _nxrt_vpx_roots "$ENV{VCPKG_ROOT}/installed/x64-windows-static")
endif()
list(TRANSFORM _nxrt_vpx_roots APPEND "/include" OUTPUT_VARIABLE _nxrt_vpx_inc)
list(TRANSFORM _nxrt_vpx_roots APPEND "/lib" OUTPUT_VARIABLE _nxrt_vpx_lib)
find_path(NXRT_LIBVPX_INCLUDE vpx/vpx_decoder.h PATHS ${_nxrt_vpx_inc} NO_DEFAULT_PATH)
find_library(NXRT_LIBVPX_LIB NAMES vpx vpxmt PATHS ${_nxrt_vpx_lib} NO_DEFAULT_PATH)
if(NXRT_LIBVPX_INCLUDE AND NXRT_LIBVPX_LIB)
    message(STATUS "nxrt: libvpx ${NXRT_LIBVPX_LIB} (host VP9/VP8 decoders)")
    target_include_directories(nxrt SYSTEM PRIVATE "${NXRT_LIBVPX_INCLUDE}")
    target_link_libraries(nxrt PUBLIC "${NXRT_LIBVPX_LIB}")
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_LIBVPX=1)
else()
    message(STATUS "nxrt: libvpx not found (looked in: ${_nxrt_vpx_roots}): no host video decoders")
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_LIBVPX=0)
endif()

# dav1d (BSD-2-Clause) for the AV1 decoder (Ocarina of Time's movies are AV1), from the same vcpkg install: vcpkg install dav1d:x64-windows-static.
# Used only together with libvpx (the decoders share video_codec.cpp's host side).
find_path(NXRT_DAV1D_INCLUDE dav1d/dav1d.h PATHS ${_nxrt_vpx_inc} NO_DEFAULT_PATH)
find_library(NXRT_DAV1D_LIB NAMES dav1d libdav1d PATHS ${_nxrt_vpx_lib} NO_DEFAULT_PATH)
if(NXRT_LIBVPX_INCLUDE AND NXRT_LIBVPX_LIB AND NXRT_DAV1D_INCLUDE AND NXRT_DAV1D_LIB)
    message(STATUS "nxrt: dav1d ${NXRT_DAV1D_LIB} (host AV1 decoder)")
    target_include_directories(nxrt SYSTEM PRIVATE "${NXRT_DAV1D_INCLUDE}")
    target_link_libraries(nxrt PUBLIC "${NXRT_DAV1D_LIB}")
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_DAV1D=1)
else()
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_DAV1D=0)
endif()
# libopus (BSD-3-Clause) for hwopus (src/services/misc.cpp, IHardwareOpusDecoder), from the same vcpkg install: vcpkg install opus:x64-windows-static.
# Without it the Opus decoders decode silence (OoT's movie and menu sounds are Opus).
find_path(NXRT_OPUS_INCLUDE opus/opus.h PATHS ${_nxrt_vpx_inc} NO_DEFAULT_PATH)
find_library(NXRT_OPUS_LIB NAMES opus PATHS ${_nxrt_vpx_lib} NO_DEFAULT_PATH)
if(NXRT_OPUS_INCLUDE AND NXRT_OPUS_LIB)
    message(STATUS "nxrt: libopus ${NXRT_OPUS_LIB} (hwopus)")
    target_include_directories(nxrt SYSTEM PRIVATE "${NXRT_OPUS_INCLUDE}")
    target_link_libraries(nxrt PUBLIC "${NXRT_OPUS_LIB}")
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_OPUS=1)
else()
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_OPUS=0)
endif()
