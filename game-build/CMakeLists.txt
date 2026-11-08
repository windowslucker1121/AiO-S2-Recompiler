# SPDX-License-Identifier: GPL-3.0-or-later
# The game exe: the runtime (../nx/runtime) + the lifted game libraries (built by ../nx/recomp/scripts/build_game.cmd <id> into ../work/build/<id>).
#   pwsh scripts\build.ps1            configure + build into game-build\build\release
# Everything game-specific (id, exe and DLL names, modules, load addresses, product name) comes from the game manifest,
# recomp/games/<id>/game.toml (GAME_MANIFEST; found next to the runtime when exactly one game has one).
# Nothing derived from the game lives here: the libraries are imported from the work folder, the NSOs are read at run time.
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

get_filename_component(GAME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# GAME_RUNTIME_DIR: which runtime checkout compiles in (default the main one; a worktree such as nx-wt/<name>/runtime for branch work)
set(GAME_RUNTIME_DIR "${GAME_ROOT}/runtime" CACHE PATH "runtime source tree compiled into the game exe")
get_filename_component(GAME_RECOMP_DIR "${GAME_RUNTIME_DIR}/../recomp" ABSOLUTE)
include("${GAME_RECOMP_DIR}/cmake/GameManifest.cmake")
set(GAME_MANIFEST "" CACHE FILEPATH "game manifest (recomp/games/<id>/game.toml); empty: the only one next to the runtime")
if(NOT GAME_MANIFEST)
    file(GLOB _gm_found "${GAME_RECOMP_DIR}/games/*/game.toml")
    list(LENGTH _gm_found _gm_n)
    if(NOT _gm_n EQUAL 1)
        message(FATAL_ERROR "set GAME_MANIFEST: ${_gm_n} game manifests found under ${GAME_RECOMP_DIR}/games")
    endif()
    set(GAME_MANIFEST "${_gm_found}" CACHE FILEPATH "game manifest" FORCE)
endif()
game_manifest_load("${GAME_MANIFEST}")
project(${GM_ID}_game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GAME_LIB_DIR "${GAME_ROOT}/work/build/${GM_ID}" CACHE PATH "folder with the lifted module libraries <id>_<module>.lib (built by recomp scripts build_game.cmd <id>)")
set(GAME_EXEFS_DEFAULT "${GAME_ROOT}/game/dump/exefs" CACHE PATH "default ExeFS folder baked into the exe (--exefs / <ID>_EXEFS override it)")

# Generated from the manifest: the header every source includes, the Win32 manifest, the resource script and the DLL export list.
set(GAME_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
game_manifest_header("${GAME_GEN}/game_manifest.h")
set(GAME_ID "${GM_ID}")
set(GAME_PRODUCT "${GM_game_product}")
set(GAME_RT_STEM "${GM_RT_STEM}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/game.manifest.in" "${GAME_GEN}/game.manifest" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/game.rc" "${GAME_GEN}/game.rc" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/rt.def.in" "${GAME_GEN}/rt.def" @ONLY)

# Release packages (GAME_RELEASE=ON): the exe is a Windows GUI program (double click opens the game window only, no console; errors show in
# a message box, output still reaches a terminal it was started from), looks for ./exefs by default, and carries no local build paths
# (source paths in assert strings, the .pdb path): see the installed-layout comment in src/game_main.cpp.
# __FILE__ (the assert strings of the audio renderer) never carries the build folder, in any build: paths are written relative to the checkout.
add_compile_options("-clang:-fmacro-prefix-map=${GAME_ROOT}=.")
option(GAME_RELEASE "build the game exe as a release package binary" OFF)
if(GAME_RELEASE)
    set(GAME_GUI_SUBSYSTEM ON)
    set(GAME_EXEFS_DEFAULT "exefs")
    add_compile_options("-clang:-ffile-prefix-map=${GAME_ROOT}=.")
endif()

# The release version ("0.1.0"; "dev" for local builds). The game log, --version and the exe file properties show it, and the
# launcher (built from the runtime with the same cache value) shows it in its window. game-build/scripts/build.ps1 -Version sets it.
set(GAME_VERSION "dev" CACHE STRING "release version shown by the game exe and the launcher")
set(GAME_VER_MAJOR 0)
set(GAME_VER_MINOR 0)
set(GAME_VER_PATCH 0)
if(GAME_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)")
    set(GAME_VER_MAJOR ${CMAKE_MATCH_1})
    set(GAME_VER_MINOR ${CMAKE_MATCH_2})
    set(GAME_VER_PATCH ${CMAKE_MATCH_3})
endif()
set(NXRT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NXRT_GAMES "${GM_ID}" CACHE STRING "" FORCE)            # runtime/games/<id> only
set(NXRT_LAUNCHER_GAME "${GM_ID}" CACHE STRING "" FORCE)
set(NXRT_RECOMP_INCLUDE "${GAME_RECOMP_DIR}/include" CACHE PATH "")
# A runtime worktree without its own prebuilt ASTC transcoder (tools/textures/build) uses the main checkout's.
if(NOT DEFINED NXRT_A2B_DIR AND NOT EXISTS "${GAME_RUNTIME_DIR}/../tools/textures/build/release/astc2bc7.lib"
   AND EXISTS "${GAME_ROOT}/tools/textures/build/release/astc2bc7.lib")
    set(NXRT_A2B_DIR "${GAME_ROOT}/tools/textures/build/release" CACHE PATH "Build folder of tools/textures")
endif()
add_subdirectory("${GAME_RUNTIME_DIR}" runtime)

foreach(m IN LISTS GM_MODULES)
    add_library(${GM_ID}_${m} STATIC IMPORTED GLOBAL)
    set_target_properties(${GM_ID}_${m} PROPERTIES IMPORTED_LOCATION "${GAME_LIB_DIR}/${GM_ID}_${m}.lib")
endforeach()

# The game library (runtime/apps/setup, recomp_library.exe): the first program a player starts, one for every game. Library, requirements,
# extraction, shader preparation queue, Play. Game independent: it reads the games' manifests at run time; it links nothing of the runtime
# (the game exe does the shader work as a child process).
include(FetchContent)
FetchContent_GetProperties(vulkan_headers)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/setup.manifest.in" "${GAME_GEN}/setup.manifest" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/setup.rc" "${GAME_GEN}/setup.rc" COPYONLY)
add_executable(game_setup WIN32 "${GAME_RUNTIME_DIR}/apps/setup/setup_main.cpp" "${GAME_RUNTIME_DIR}/apps/setup/setup_core.cpp"
               "${GAME_RUNTIME_DIR}/apps/setup/setup_games.cpp" "${GAME_RUNTIME_DIR}/apps/setup/setup_settings.cpp"
               "${GAME_RUNTIME_DIR}/apps/launcher/keymap_win.cpp" "${GAME_GEN}/setup.rc")
set_target_properties(game_setup PROPERTIES OUTPUT_NAME "recomp_library")
FetchContent_GetProperties(sdl3)   # SDL_scancode.h only: the key names the settings page writes (no SDL library is linked)
target_include_directories(game_setup PRIVATE "${GAME_GEN}" "${GAME_RUNTIME_DIR}/apps/setup" "${GAME_RUNTIME_DIR}/apps/launcher"
                           "${vulkan_headers_SOURCE_DIR}/include" "${sdl3_SOURCE_DIR}/include")
target_compile_definitions(game_setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set_source_files_properties("${GAME_GEN}/setup.rc" PROPERTIES COMPILE_DEFINITIONS "GAME_VER_MAJOR=${GAME_VER_MAJOR};GAME_VER_MINOR=${GAME_VER_MINOR};GAME_VER_PATCH=${GAME_VER_PATCH};GAME_VER_TEXT=\"${GAME_VERSION}\""
                            OBJECT_DEPENDS "${GAME_GEN}/setup.manifest")
if(NOT GAME_RELEASE)
    target_compile_definitions(game_setup PRIVATE SETUP_DEV_ROOT="${GAME_ROOT}")   # development build: the checkout's tools (scripts, translator, venv)
endif()
target_link_libraries(game_setup PRIVATE comctl32 uxtheme shell32 ole32 shlwapi gdiplus dwmapi)
target_link_options(game_setup PRIVATE /MANIFEST:NO /DEBUG)   # res/setup.rc carries the manifest

# gen/build_id.h: the commit this exe (or runtime DLL) is built from and whether tracked files had changes (bug capture bundles record it, nxrt/bugcapture.h).
# Rewritten only when it changes, at every build.
find_package(Git QUIET)
set(_game_build_id_args "-DGIT=${GIT_EXECUTABLE}" "-DROOT=${GAME_ROOT}" "-DOUT=${GAME_GEN}/build_id.h" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake")
execute_process(COMMAND "${CMAKE_COMMAND}" ${_game_build_id_args})
add_custom_target(game_build_id COMMAND "${CMAKE_COMMAND}" ${_game_build_id_args} BYPRODUCTS "${GAME_GEN}/build_id.h" VERBATIM)

# Release model (b) (GAME_BUILD_RT_DLL=ON, docs/RELEASE.md): no game code in anything built here. The runtime and the boot code become
# the runtime DLL (+ the import library of the same stem); the game exe is linked on the player PC from the libraries lifted out of
# their own dump and src/stub.cpp (tools/build/game_build.py in the package, with the bundled Zig). Nothing lifted is imported here.
option(GAME_BUILD_RT_DLL "build the runtime as a DLL for release model (b)" OFF)
if(GAME_BUILD_RT_DLL)
    add_library(game_rt SHARED src/game_main.cpp "${GAME_GEN}/rt.def")
    set_target_properties(game_rt PROPERTIES OUTPUT_NAME "${GM_RT_STEM}")
    set_source_files_properties(src/game_main.cpp PROPERTIES COMPILE_DEFINITIONS "GAME_RELEASE_VERSION=\"${GAME_VERSION}\"")
    target_include_directories(game_rt PRIVATE "${GAME_GEN}" src)
    target_compile_definitions(game_rt PRIVATE GAME_AS_DLL=1 GAME_GUI_SUBSYSTEM=1 GAME_EXEFS_DEFAULT="exefs")
    target_link_libraries(game_rt PRIVATE nxrt_game_${GM_ID} nxrt_gfx nxrt)
    add_dependencies(game_rt game_build_id)
    # /DEBUG off: no .pdb path in the DLL. Low host allocations as for the exe (guest ranges at 0x71_0000_0000 stay free).
    target_link_options(game_rt PRIVATE /HIGHENTROPYVA:NO /DEBUG:NONE /Brepro)
    target_compile_options(game_rt PRIVATE "-clang:-ffile-prefix-map=${GAME_ROOT}=.")
    add_custom_command(TARGET game_rt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GAME_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:game_rt>/fonts/DejaVuSans.ttf")
    file(GLOB GAME_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")
    foreach(dll ${GAME_A2B_DLLS})
        add_custom_command(TARGET game_rt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:game_rt>")
    endforeach()
    return()
endif()

add_executable(game_exe src/game_main.cpp "${GAME_GEN}/game.rc")
add_dependencies(game_exe game_build_id)
set_target_properties(game_exe PROPERTIES OUTPUT_NAME "${GM_EXE_STEM}")
set_source_files_properties(src/game_main.cpp PROPERTIES COMPILE_DEFINITIONS "GAME_RELEASE_VERSION=\"${GAME_VERSION}\"")
set_source_files_properties("${GAME_GEN}/game.rc" PROPERTIES COMPILE_DEFINITIONS "GAME_VER_MAJOR=${GAME_VER_MAJOR};GAME_VER_MINOR=${GAME_VER_MINOR};GAME_VER_PATCH=${GAME_VER_PATCH};GAME_VER_TEXT=\"${GAME_VERSION}\""
                            OBJECT_DEPENDS "${GAME_GEN}/game.manifest")
target_include_directories(game_exe PRIVATE "${GAME_GEN}")
target_compile_definitions(game_exe PRIVATE GAME_EXEFS_DEFAULT="${GAME_EXEFS_DEFAULT}")
# Development builds know their checkout: started without --game they run game/<id>/updates/<newest> with work/shaders/<id> (game_main.cpp).
if(NOT GAME_RELEASE)
    target_compile_definitions(game_exe PRIVATE GAME_DEV_ROOT="${GAME_ROOT}")
endif()
target_link_libraries(game_exe PRIVATE ${GM_MODULE_LIBS} nxrt_game_${GM_ID} nxrt_gfx nxrt)   # nxrt_game_<id>: runtime/games/<id>
# Same link options as the runtime test executables: a linker map next to the exe (host frames get names), a big main stack (lifted
# code nests deeply), low host allocations so the guest ranges at 0x71_0000_0000 and up stay free.
if(GAME_RELEASE)
    target_compile_definitions(game_exe PRIVATE GAME_GUI_SUBSYSTEM=1)
    target_link_options(game_exe PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /PDBALTPATH:${GM_ID}.pdb)
endif()
target_link_options(game_exe PRIVATE /MAP /STACK:134217728 /HIGHENTROPYVA:NO /DEBUG /MANIFEST:NO)   # res/game.rc carries the manifest

# The ASTC decoder DLLs astc2bc7 loads at run time (NVN textures in ASTC are transcoded to BC7 on first use): next to the exe.
# (NXRT_A2B_DIR is the tools/textures build folder the runtime links its transcoder from: cmake/gfx.cmake)
# The shared-font fallback (runtime src/services/pl.cpp looks in fonts\ next to the exe): without any font the game crashes at boot.
add_custom_command(TARGET game_exe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${GAME_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:game_exe>/fonts/DejaVuSans.ttf")

# Keyboard and mouse prompt icons (the game module reads assets\kbm_icons\<name>.png next to the exe): Kenney Input Prompts
# (CC0), the "Keyboard & Mouse/Default" PNGs, kept in the work folder (not in any repository). Without them the prompts show key names as text.
set(GAME_KBM_ICONS_DIR "${GAME_ROOT}/work/assets/kenney_input_prompts/Keyboard & Mouse/Default" CACHE PATH "Kenney Input Prompts PNG folder for assets/kbm_icons")
if(EXISTS "${GAME_KBM_ICONS_DIR}")
    # a script copies them: the folder name has an '&', which the command lines of the build (cmd) cannot carry
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/copy_kbm_icons.cmake"
        "file(GLOB _png \"${GAME_KBM_ICONS_DIR}/*.png\")
file(COPY \${_png} DESTINATION \"\${DST}\")
")
    add_custom_target(game_kbm_icons COMMAND ${CMAKE_COMMAND} "-DDST=$<TARGET_FILE_DIR:game_exe>/assets/kbm_icons" -P "${CMAKE_CURRENT_BINARY_DIR}/copy_kbm_icons.cmake" VERBATIM)
    add_dependencies(game_exe game_kbm_icons)
endif()

file(GLOB GAME_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")
foreach(dll ${GAME_A2B_DLLS})
    add_custom_command(TARGET game_exe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:game_exe>")
endforeach()
