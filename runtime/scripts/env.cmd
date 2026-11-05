@echo off
rem Sets up the MSVC + LLVM + CMake + Ninja (+ sccache) environment for the runtime build (call it, then run cmake/ninja): the same as
rem recomp\scripts\nx_env.cmd, which finds Visual Studio with vswhere and prefers the LLVM and sccache unpacked under tools\.
call "%~dp0..\..\recomp\scripts\nx_env.cmd" >nul
