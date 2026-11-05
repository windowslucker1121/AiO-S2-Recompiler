# nx_extract: raw dump files (NSP / XCI + the player's keys) -> extracted ExeFS / RomFS, and ZBIC NSOs -> plain NSOs. Game independent.
# Windows x64, clang-cl + Ninja:  pwsh -File tools\extract\scripts\build.ps1 [-Test]
# Reuses the container / NCA / key / update readers of tools/setup/src and the runtime's ZBIC decoder (runtime/src/loader/zbic.cpp).
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: players need no redistributable
endif()
project(nx_extract C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
enable_testing()

set(SETUP ${CMAKE_CURRENT_SOURCE_DIR}/../setup)
set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime)
set(SS ${SETUP}/src)

# nx_plain_nso: --plain-nso alone (ZBIC NSO -> plain NSO), needs only the runtime's ZBIC decoder
add_executable(nx_plain_nso cli/plain_nso.cpp ${RT}/src/loader/zbic.cpp)
target_include_directories(nx_plain_nso PRIVATE ${RT}/src/loader)
target_compile_definitions(nx_plain_nso PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(nx_plain_nso PRIVATE bcrypt)

# the rest reuses tools/setup, which this repository does not carry
if(NOT EXISTS ${SS}/util.cpp)
    message(STATUS "tools/setup not found: building nx_plain_nso only")
    return()
endif()

add_library(nxx_core STATIC
    ${SS}/util.cpp ${SS}/storage.cpp ${SS}/log.cpp ${SS}/crypto.cpp ${SS}/fileio.cpp ${SS}/pfs.cpp ${SS}/romfs.cpp
    ${SS}/keys.cpp ${SS}/bktr.cpp ${SS}/nca.cpp ${SS}/container.cpp ${SS}/nso.cpp ${SS}/ncz.cpp
    ${RT}/src/loader/zbic.cpp
    src/game_any.cpp src/extract.cpp)
target_include_directories(nxx_core PUBLIC ${SS} ${CMAKE_CURRENT_SOURCE_DIR}/src ${RT}/src/loader)
target_compile_definitions(nxx_core PUBLIC _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE MPRS_EXE_NAME="nx_extract" MPR_VERSION="dev")
target_compile_options(nxx_core PRIVATE /W3 -Wno-unused-parameter)
target_link_libraries(nxx_core PUBLIC bcrypt shlwapi ole32 shell32)

# NSZ / XCZ: plain zstd 1.5.7 (BSD-3-Clause) when tools/setup/scripts/fetch_zstd.ps1 has fetched it. Its symbols do not meet the ZBIC
# decoder's, which zbic.cpp keeps internal.
if(EXISTS ${SETUP}/third_party/zstd/lib/zstd.h)
    file(GLOB ZSTD_SRC ${SETUP}/third_party/zstd/lib/common/*.c ${SETUP}/third_party/zstd/lib/decompress/*.c)
    add_library(nxx_zstd STATIC ${ZSTD_SRC})
    target_include_directories(nxx_zstd PUBLIC ${SETUP}/third_party/zstd/lib)
    target_compile_definitions(nxx_zstd PRIVATE ZSTD_DISABLE_ASM=1 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(nxx_core PUBLIC nxx_zstd)
    target_compile_definitions(nxx_core PUBLIC MPRS_HAVE_ZSTD=1)
endif()

add_executable(nx_extract cli/main.cpp)
target_link_libraries(nx_extract PRIVATE nxx_core)

# test: synthetic NSP / XCI (tools/setup/tests/synth*.cpp builders, fake keys) through extract_dump, and a ZBIC NSO through nso_to_plain
add_executable(test_extract tests/test_extract.cpp ${SETUP}/tests/synth.cpp ${SETUP}/tests/synth_nca.cpp)
target_include_directories(test_extract PRIVATE ${SETUP}/tests)
target_link_libraries(test_extract PRIVATE nxx_core)
add_test(NAME extract COMMAND test_extract)
