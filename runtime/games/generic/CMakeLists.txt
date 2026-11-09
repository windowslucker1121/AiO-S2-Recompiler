# SPDX-License-Identifier: GPL-3.0-or-later
# The generic game module (generic_game.h): the runtime of experimental builds of titles without a port. Libraries as games/oot, each a
# superset of the one before:
#   nxrt_game_generic_data   identity, branding (read from the game manifest at start)
#   nxrt_game_generic_core   + title facts (links nxrt)
#   nxrt_game_generic        + play mode (links nxrt_gfx; only with NXRT_BUILD_GFX). generic_rt.dll links this one.
set(_gen "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")
set(NXRT_GAME_GENERIC_INCLUDES "${_rt}/include" "${_rt}/games" "${_gen}" "${_rt}/src/gfx" "${_rt}/src/gfx/play" "${_rt}/src/testtools")

add_library(nxrt_game_generic_data STATIC "${_gen}/generic_data.cpp")
target_include_directories(nxrt_game_generic_data PUBLIC ${NXRT_GAME_GENERIC_INCLUDES})
target_compile_features(nxrt_game_generic_data PUBLIC cxx_std_20)

add_library(nxrt_game_generic_core STATIC "${_gen}/generic_core.cpp")
target_link_libraries(nxrt_game_generic_core PUBLIC nxrt_game_generic_data nxrt)

if(TARGET nxrt_gfx)
    add_library(nxrt_game_generic STATIC "${_gen}/generic_game.cpp")
    target_link_libraries(nxrt_game_generic PUBLIC nxrt_game_generic_core nxrt_gfx)
endif()
