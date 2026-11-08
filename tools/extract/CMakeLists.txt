# nx_plain_nso: ZBIC NSOs -> plain NSOs (the lifter reads plain ones). Game independent; reads already-decrypted ExeFS files only.
# Windows x64, clang-cl + Ninja:  powershell -File tools\extract\scripts\build.ps1 [-Test]
# Uses the runtime's ZBIC decoder (runtime/src/loader/zbic.cpp). Nothing here reads keys or decrypts containers.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: players need no redistributable
endif()
project(nx_plain_nso C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
enable_testing()

set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime)

add_executable(nx_plain_nso cli/plain_nso.cpp ${RT}/src/loader/zbic.cpp)
target_include_directories(nx_plain_nso PRIVATE ${RT}/src/loader)
target_compile_definitions(nx_plain_nso PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(nx_plain_nso PRIVATE bcrypt)

# test: a synthetic ZBIC NSO (tests/zbic_frames.inc) through nx_plain_nso.exe
add_executable(test_plain_nso tests/test_plain_nso.cpp)
target_compile_definitions(test_plain_nso PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(test_plain_nso PRIVATE bcrypt)
add_test(NAME plain_nso COMMAND test_plain_nso $<TARGET_FILE:nx_plain_nso>)
