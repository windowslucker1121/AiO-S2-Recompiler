# SPDX-License-Identifier: GPL-3.0-or-later
# Toolchain file: LLVM clang-cl (C:\Program Files\LLVM\bin) with the MSVC x64 environment (run through scripts/nx_env.cmd,
# or from a shell where vcvars64.bat has run), Ninja, optional sccache.
set(CMAKE_SYSTEM_NAME Windows)
# an LLVM release archive unpacked under tools/llvm/ in this repository wins over the installed one
file(GLOB _nx_llvm "${CMAKE_CURRENT_LIST_DIR}/../../tools/llvm/*/bin/clang-cl.exe")
if(_nx_llvm)
  list(GET _nx_llvm 0 _nx_llvm)
  get_filename_component(NX_LLVM_BIN "${_nx_llvm}" DIRECTORY)
else()
  set(NX_LLVM_BIN "C:/Program Files/LLVM/bin")
endif()
set(CMAKE_C_COMPILER "${NX_LLVM_BIN}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${NX_LLVM_BIN}/clang-cl.exe")
set(CMAKE_LINKER "${NX_LLVM_BIN}/lld-link.exe")
set(CMAKE_RC_COMPILER "${NX_LLVM_BIN}/llvm-rc.exe")
set(CMAKE_MT "${NX_LLVM_BIN}/llvm-mt.exe")
# static MSVC runtime (/MT): the lifted libraries and the runtime that links them must agree, and players then need no
# Visual C++ Redistributable (CMake needs policy CMP0091 NEW, i.e. cmake_minimum_required 3.15 or later, for this to apply)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# the debug info format of an exe built here is not useful to the oracles: keep objects small
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
if(NOT DEFINED ENV{NX_NO_SCCACHE})
  find_program(NX_SCCACHE sccache HINTS "${CMAKE_CURRENT_LIST_DIR}/../../tools/sccache" "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links")
  if(NX_SCCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${NX_SCCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${NX_SCCACHE}")
  endif()
endif()
