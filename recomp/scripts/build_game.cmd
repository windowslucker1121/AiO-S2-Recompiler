@echo off
rem Configure and build the module libraries of one game (games\<id>), sccache kept in work\build\sccache.
rem   scripts\build_game.cmd <id> [extra ninja args, e.g. -j 20 | -k 0 | <id>_sdk]
rem A game folder games\<id> is a CMake project that takes NX_ROOT (the repository root: game\, work\) and has a target <id>_modules.
rem Environment: NX_JOBS (default 20) = compile processes; NX_BUILD (default <root>\work\build\<id>); NX_ISA = target ISA of the
rem lifted code (x86-64-v3 by default; baseline for release builds that must run on CPUs without AVX2; kept in the build cache);
rem NX_CMAKE_ARGS = more configure arguments (e.g. -DOOT_EXEFS=<folder> to read the plain NSOs of another checkout).
setlocal
if "%~1"=="" (
    echo usage: scripts\build_game.cmd ^<id^> [ninja args]
    exit /b 2
)
rem the game id without quotes, and the arguments after it (cmd's shift does not change %*)
set "GAME=%~1"
set "ARGS=%*"
call set "ARGS=%%ARGS:*%1=%%"
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not exist "%HERE%..\games\%GAME%\CMakeLists.txt" (
    echo no game folder games\%GAME%
    exit /b 2
)
if not defined NX_BUILD set "NX_BUILD=%ROOT%\work\build\%GAME%"
if not defined NX_JOBS set "NX_JOBS=20"
if not defined SCCACHE_DIR set "SCCACHE_DIR=%ROOT%\work\build\sccache"
if not defined SCCACHE_SERVER_PORT set "SCCACHE_SERVER_PORT=24339"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=20G"
set "ISA_ARG="
if defined NX_ISA set "ISA_ARG=-DNX_ISA=%NX_ISA%"
call "%HERE%nx_env.cmd" cmake -S "%HERE%..\games\%GAME%" -B "%NX_BUILD%" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%HERE%..\cmake\clang-cl.cmake" -DCMAKE_BUILD_TYPE=Release -DNX_ROOT="%ROOT%" %ISA_ARG% %NX_CMAKE_ARGS%
if errorlevel 1 exit /b 1
call "%HERE%nx_env.cmd" cmake --build "%NX_BUILD%" --target %GAME%_modules -- -j %NX_JOBS%%ARGS%
exit /b %errorlevel%
