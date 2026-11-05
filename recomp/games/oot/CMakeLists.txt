# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lift the NSOs of the user's dump and build one static library per module (as games/pkp). Everything game-specific comes from
# game.toml in this folder.
#
#   recomp\scripts\build_game.cmd oot          (configure + build into work\build\oot; NX_ROOT is the repository root)
#
# Inputs (read-only): <root>\game\oot\plain\exefs\<module files>, the plain NSOs of the dump:
#   python tools\extract\nup_extract.py <game.nup> game\oot --exefs-only
#   tools\extract\build\msvc\bin\nx_plain_nso.exe game\oot\updates\131072\exefs game\oot\plain\exefs
# Generated C++ goes to <root>\work\lift\oot\<module>, build output to work\build\oot: neither is ever committed.
cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/GameManifest.cmake")
game_manifest_load("${CMAKE_CURRENT_LIST_DIR}/game.toml")
project(${GM_ID}_lift C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NxRecomp.cmake")

string(TOUPPER "${GM_ID}" _G)
# this repository carries recomp/ at its root (game/ and work/ next to it), so the root is three levels up, or NX_ROOT itself
get_filename_component(_ROOT_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(DEFINED NX_ROOT)
    get_filename_component(_ROOT_DEFAULT "${NX_ROOT}" ABSOLUTE)
endif()
set(${_G}_ROOT "${_ROOT_DEFAULT}" CACHE PATH "repository root (game/, work/)")
set(${_G}_EXEFS "${${_G}_ROOT}/game/${GM_ID}/plain/exefs" CACHE PATH "folder with the plain NSOs of the game")
set(${_G}_GEN "${${_G}_ROOT}/work/lift/${GM_ID}" CACHE PATH "where the generated sources go")
math(EXPR _last "${GM_MODULE_COUNT} - 1")
foreach(_i RANGE ${_last})
    set(_m "${GM_module_${_i}_name}")
    string(TOUPPER "${_m}" _M)
    set(${_G}_SHARDS_${_M} ${GM_module_${_i}_units} CACHE STRING "translation units of ${_m}")
    set(_sites "")
    if(DEFINED GM_module_${_i}_sites)
        # one cache entry per module: a shared one keeps the first module's file for all of them
        set(${_G}_SITES_${_M} "${CMAKE_CURRENT_LIST_DIR}/${GM_module_${_i}_sites}" CACHE FILEPATH "hook sites of ${_m}")
        set(_sites SITES "${${_G}_SITES_${_M}}")
    endif()
    nxrecomp_add_module(${GM_ID}_${_m} INPUT "${${_G}_EXEFS}/${GM_module_${_i}_file}" NAME ${_m} INDEX ${GM_module_${_i}_index}
                        BASE ${GM_module_${_i}_base} GAME ${GM_ID} SHARDS ${${_G}_SHARDS_${_M}}
                        ${_sites} OUTDIR "${${_G}_GEN}/${_m}")
endforeach()
add_custom_target(${GM_ID}_modules DEPENDS ${GM_MODULE_LIBS})
