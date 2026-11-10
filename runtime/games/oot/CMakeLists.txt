# SPDX-License-Identifier: GPL-3.0-or-later
# The Ocarina of Time game module (docs/GAME_BOUNDARY.md, oot_game.h). Libraries, each a superset of the one before:
#   nxrt_game_oot_data   identity, branding (pure data; the library links it)
#   nxrt_game_oot_core   + title facts (links nxrt)
#   nxrt_game_oot        + play-mode hooks (links nxrt_gfx; only with NXRT_BUILD_GFX). oot.exe links this one.
set(_oot "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")
# Identity and file names come from the game manifest (recomp/games/oot/game.toml) through a generated header (oot_data.cpp).
get_filename_component(_oot_recomp "${_rt}/../recomp" ABSOLUTE)
include("${_oot_recomp}/cmake/GameManifest.cmake")
game_manifest_load("${_oot_recomp}/games/oot/game.toml")
set(_oot_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
game_manifest_header("${_oot_gen}/game_manifest.h")
set(NXRT_GAME_OOT_INCLUDES "${_rt}/include" "${_rt}/games" "${_oot}" "${_rt}/src/gfx" "${_rt}/src/gfx/play" "${_rt}/src/testtools")

add_library(nxrt_game_oot_data STATIC "${_oot}/oot_data.cpp")
target_include_directories(nxrt_game_oot_data PUBLIC ${NXRT_GAME_OOT_INCLUDES})
target_include_directories(nxrt_game_oot_data PRIVATE "${_oot_gen}")
target_compile_features(nxrt_game_oot_data PUBLIC cxx_std_20)

add_library(nxrt_game_oot_core STATIC "${_oot}/oot_core.cpp")
target_link_libraries(nxrt_game_oot_core PUBLIC nxrt_game_oot_data nxrt)

if(TARGET nxrt_gfx)
    add_library(nxrt_game_oot STATIC "${_oot}/oot_game.cpp" "${_oot}/oot_drs.cpp")
    target_link_libraries(nxrt_game_oot PUBLIC nxrt_game_oot_core nxrt_gfx)
endif()
