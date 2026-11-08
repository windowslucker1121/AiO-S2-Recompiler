# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone build of the game library exe (runtime/apps/setup, recomp_library.exe) for release packages and CI.
# Unlike game-build (target game_setup) it needs no game, no lifted libraries and no runtime: only the Vulkan headers (the GPU check)
# and SDL's scancode header (key names), fetched with the same pins and hashes as runtime/cmake/gfx_deps.cmake.
#   cmake -S tools/release/library -B work/build/release_library -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl
#         -DLIBRARY_VERSION=0.1.0 [-DLIBRARY_DEV_ROOT=ON]
cmake_minimum_required(VERSION 3.25)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT: no VC++ redistributable needed
project(recomp_library CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LIBRARY_VERSION "dev" CACHE STRING "version shown in the exe's file properties")
option(LIBRARY_DEV_ROOT "use this checkout's tools (scripts, translator, venv) as a fallback; off for release packages" OFF)

include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
set(DEPS_DIR "$ENV{LOCALAPPDATA}/nxrt/deps" CACHE PATH "download cache of the pinned archives")
# Same archives as runtime/cmake/gfx_deps.cmake (keep the pins in step). Headers only: nothing is built or linked.
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    DOWNLOAD_DIR "${DEPS_DIR}"
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    SOURCE_SUBDIR none)
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${DEPS_DIR}"
    URL_HASH SHA256=f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(sdl3 vulkan_headers)

set(VER_MAJOR 0)
set(VER_MINOR 0)
set(VER_PATCH 0)
if(LIBRARY_VERSION MATCHES "^v?([0-9]+)[.]([0-9]+)[.]([0-9]+)")
    set(VER_MAJOR ${CMAKE_MATCH_1})
    set(VER_MINOR ${CMAKE_MATCH_2})
    set(VER_PATCH ${CMAKE_MATCH_3})
endif()

set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
configure_file("${REPO}/game-build/res/setup.manifest.in" "${GEN}/setup.manifest" @ONLY)
configure_file("${REPO}/game-build/res/setup.rc" "${GEN}/setup.rc" COPYONLY)

set(APP "${REPO}/runtime/apps")
add_executable(recomp_library WIN32
    "${APP}/setup/setup_main.cpp" "${APP}/setup/setup_core.cpp" "${APP}/setup/setup_games.cpp" "${APP}/setup/setup_settings.cpp"
    "${APP}/launcher/keymap_win.cpp" "${GEN}/setup.rc")
target_include_directories(recomp_library PRIVATE "${GEN}" "${APP}/setup" "${APP}/launcher"
                           "${vulkan_headers_SOURCE_DIR}/include" "${sdl3_SOURCE_DIR}/include")
target_compile_definitions(recomp_library PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                           RECOMP_LIBRARY_VERSION="${LIBRARY_VERSION}")
if(LIBRARY_DEV_ROOT)
    target_compile_definitions(recomp_library PRIVATE SETUP_DEV_ROOT="${REPO}")
endif()
set_source_files_properties("${GEN}/setup.rc" PROPERTIES
    COMPILE_DEFINITIONS "GAME_VER_MAJOR=${VER_MAJOR};GAME_VER_MINOR=${VER_MINOR};GAME_VER_PATCH=${VER_PATCH};GAME_VER_TEXT=\"${LIBRARY_VERSION}\""
    OBJECT_DEPENDS "${GEN}/setup.manifest")
# No build paths in the exe: assert strings relative to the checkout, no .pdb path, reproducible link.
target_compile_options(recomp_library PRIVATE "$<$<CXX_COMPILER_ID:Clang>:-clang:-ffile-prefix-map=${REPO}=.>")
target_link_libraries(recomp_library PRIVATE comctl32 uxtheme shell32 ole32 shlwapi gdiplus dwmapi user32 gdi32 advapi32)
target_link_options(recomp_library PRIVATE /MANIFEST:NO /DEBUG:NONE /Brepro)
