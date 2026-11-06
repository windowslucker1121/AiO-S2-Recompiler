# SPDX-License-Identifier: GPL-3.0-or-later
# Graphics shell (S4b): SDL3 platform + input queue, Vulkan 1.3 device layer, NVN front end, ImGui overlay.
# Built as the static library nxrt_gfx (links nxrt) and the test executable nxrt_gfx_tests (CTest groups gfx_*).
include(${CMAKE_CURRENT_LIST_DIR}/gfx_deps.cmake)

# The offline tiling library of the tools repository is compiled in as source (tools/tiling, tegra_tiling.h): found at
# ../tools or, from a worktree under runtime-wt/<name>, at ../../tools.
set(NXRT_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools" CACHE PATH "The tools repository (tools/tiling is used)")
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/tiling/src/tegra_tiling.cpp")
    get_filename_component(_nxrt_td "${CMAKE_CURRENT_SOURCE_DIR}/../../tools" ABSOLUTE)
    set(NXRT_TOOLS_DIR "${_nxrt_td}" CACHE PATH "The tools repository (tools/tiling is used)" FORCE)
endif()
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
    message(FATAL_ERROR "tools/tiling not found: set NXRT_TOOLS_DIR to the tools repository (looked in ${NXRT_TOOLS_DIR})")
endif()
add_library(nxrt_tiling STATIC "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
target_include_directories(nxrt_tiling PUBLIC "${NXRT_TOOLS_DIR}/tiling/include")

# ASTC -> BC7 transcoder (tools/textures): the prebuilt static libraries of the tools repository (scripts/build.ps1 there) and the
# decoder DLLs that astc2bc7 loads at run time (copied next to every executable that links nxrt_gfx).
set(NXRT_A2B_DIR "${NXRT_TOOLS_DIR}/textures/build/release" CACHE PATH "Build folder of tools/textures (astc2bc7.lib, bc7e_ispc.lib, astcdec-*.dll)")
if(NOT EXISTS "${NXRT_A2B_DIR}/astc2bc7.lib" OR NOT EXISTS "${NXRT_A2B_DIR}/bc7e_ispc.lib")
    message(FATAL_ERROR "astc2bc7.lib not found in ${NXRT_A2B_DIR}: build tools/textures first (pwsh tools/textures/scripts/build.ps1) or set NXRT_A2B_DIR")
endif()
add_library(nxrt_astc2bc7 INTERFACE)
target_include_directories(nxrt_astc2bc7 INTERFACE "${NXRT_TOOLS_DIR}/textures/include")
target_link_libraries(nxrt_astc2bc7 INTERFACE "${NXRT_A2B_DIR}/astc2bc7.lib" "${NXRT_A2B_DIR}/bc7e_ispc.lib")
file(GLOB NXRT_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")

file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "nvn_gpu_stub[.]cpp$")   # unit-test stand-in of the GPU boundary (scripts/nvn_unit.ps1)
add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES})
# The About page of the F1 menu shows the commit this build was made from (read when CMake configures; absent without git).
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE NXRT_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _nxrt_git_rc)
    if(_nxrt_git_rc EQUAL 0 AND NXRT_BUILD_COMMIT)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/play/play_ui_about.cpp" PROPERTIES COMPILE_DEFINITIONS "NXRT_BUILD_COMMIT=\"${NXRT_BUILD_COMMIT}\"")
    endif()
endif()
target_include_directories(nxrt_gfx PUBLIC include src/gfx/nvn/third_party PRIVATE src/gfx src/gfx/nvn)
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling nxrt_astc2bc7 SDL3::SDL3-static)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

add_executable(nxrt_gfx_demo apps/gfx_demo.cpp)
target_link_libraries(nxrt_gfx_demo PRIVATE nxrt_gfx)
target_link_options(nxrt_gfx_demo PRIVATE /MAP /DEBUG /STACK:8388608)
foreach(dll ${NXRT_A2B_DLLS})
    add_custom_command(TARGET nxrt_gfx_demo POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_demo>")
endforeach()

if(NXRT_BUILD_TESTS)
    file(GLOB_RECURSE NXRT_GFX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx/*.cpp")
    # the button prompt tests read icons with the Metroid Prime module's pak/txtr readers (games/mpr), not in every checkout
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/mpr/CMakeLists.txt")
        list(FILTER NXRT_GFX_TEST_SOURCES EXCLUDE REGEX "/tests/gfx/test_prompts\\.cpp$")
    endif()
    # The tests of the registered test game's own code (games/<id>/tests) run in the same executable (docs/GAME_BOUNDARY.md).
    if(NXRT_TEST_GAME)
        file(GLOB NXRT_GAME_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${NXRT_TEST_GAME}/tests/*.cpp")
        list(APPEND NXRT_GFX_TEST_SOURCES ${NXRT_GAME_TEST_SOURCES})
    endif()
    add_executable(nxrt_gfx_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp" ${NXRT_GFX_TEST_SOURCES})
    target_include_directories(nxrt_gfx_tests PRIVATE tests src/gfx src/gfx/nvn)
    target_link_libraries(nxrt_gfx_tests PRIVATE nxrt_gfx)
    target_compile_definitions(nxrt_gfx_tests PRIVATE NXRT_GFX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/gfx")
    # The BC7 reference decoder of bc7enc_rdo (public domain; the same file the setup tool checks its texture cache with): the ASTC texture scene
    # works out its expected picture from the current encoder instead of a stored image (tests/gfx/test_nvn_scene_texture.cpp).
    set(NXRT_BC7_REF_DIR "${NXRT_TOOLS_DIR}/textures/third_party/bc7enc_rdo")
    if(EXISTS "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
        target_sources(nxrt_gfx_tests PRIVATE "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
        target_include_directories(nxrt_gfx_tests PRIVATE "${NXRT_BC7_REF_DIR}")
        target_compile_definitions(nxrt_gfx_tests PRIVATE NXRT_HAVE_BC7_REF=1)
    endif()
    target_link_options(nxrt_gfx_tests PRIVATE /MAP /STACK:8388608 /DEBUG)
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET nxrt_gfx_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_tests>")
    endforeach()

    set(NXRT_GFX_GROUPS "")
    foreach(src ${NXRT_GFX_TEST_SOURCES})
        file(STRINGS "${src}" lines REGEX "^NXTEST_GROUP[(]")
        foreach(l ${lines})
            string(REGEX REPLACE "^NXTEST_GROUP[(]([a-z0-9_]+)[)].*$" "\\1" g "${l}")
            list(APPEND NXRT_GFX_GROUPS ${g})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES NXRT_GFX_GROUPS)
    foreach(g ${NXRT_GFX_GROUPS})
        add_test(NAME ${g} COMMAND nxrt_gfx_tests ${g} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${g} PROPERTIES TIMEOUT 300 ENVIRONMENT "NXRT_GFX_HIDDEN=1")
    endforeach()
    # Timing oracles must not share the machine with another test of this suite.
    foreach(g gfx_limiter gfx_pacing)
        if(TEST ${g})
            set_tests_properties(${g} PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()
    if(TEST gfx_pacing)
        set_tests_properties(gfx_pacing PROPERTIES TIMEOUT 900)   # up to four attempts per case on a busy GPU
    endif()
    message(STATUS "nxrt gfx test groups: ${NXRT_GFX_GROUPS}")
endif()

# The launcher (apps/launcher).
include(${CMAKE_CURRENT_LIST_DIR}/launcher.cmake)
