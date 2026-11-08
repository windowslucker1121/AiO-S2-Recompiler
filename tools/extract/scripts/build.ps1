# Configure + build nx_plain_nso (and its test) with CMake + Ninja + clang-cl; -Test runs the test.
#   powershell -ExecutionPolicy Bypass -File tools\extract\scripts\build.ps1 [-Clean] [-Test] [-BuildDir path]
# Output: <BuildDir>\bin\nx_plain_nso.exe (default tools\extract\build\release). The environment comes from recomp\scripts\nx_env.cmd
# (Visual Studio found with vswhere, the repo-local LLVM preferred).
param([switch]$Clean, [switch]$Test, [string]$BuildDir = '')
$ErrorActionPreference = 'Stop'
$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repo  = [IO.Path]::GetFullPath((Join-Path $root '..\..'))
$build = if ($BuildDir) { $BuildDir } else { Join-Path $root 'build\release' }
$env_cmd = Join-Path $repo 'recomp\scripts\nx_env.cmd'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
$cmd = "call `"$env_cmd`" && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build `"$build`""
if ($Test) { $cmd += " && ctest --test-dir `"$build`" --output-on-failure" }
cmd /c $cmd
if ($LASTEXITCODE) { throw 'build or test failed' }
Write-Output "built: $build\bin"
