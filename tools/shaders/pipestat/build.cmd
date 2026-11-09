@echo off
rem Builds pipestat.exe into tools\shaders\build\pipestat (git-ignored). Needs the game build's FetchContent folders (volk, Vulkan-Headers)
rem of this checkout or of the main checkout (PIPESTAT_DEPS=<...>\game-build\build\release\_deps) and the repo-local clang-cl.
setlocal
set HERE=%~dp0
set ROOT=%HERE%..\..\..
if "%PIPESTAT_DEPS%"=="" set PIPESTAT_DEPS=%ROOT%\game-build\build\release\_deps
set OUT=%ROOT%\tools\shaders\build\pipestat
if not exist "%OUT%" mkdir "%OUT%"
call "%ROOT%\recomp\scripts\nx_env.cmd" || exit /b 1
clang-cl /nologo /O2 /EHsc /std:c++20 /MT -I"%PIPESTAT_DEPS%\vulkan_headers-src\include" -I"%PIPESTAT_DEPS%\volk-src" ^
  "%HERE%pipestat.cpp" "%PIPESTAT_DEPS%\volk-src\volk.c" /Fo"%OUT%\\" /Fe"%OUT%\pipestat.exe" || exit /b 1
echo %OUT%\pipestat.exe
