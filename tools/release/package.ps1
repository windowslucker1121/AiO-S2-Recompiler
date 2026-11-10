# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the game library release package (recomp_library.exe and everything it runs) into a folder and a zip.
# The layout is the one runtime/apps/setup/setup_core.cpp (find_tools) and setup_games.cpp (find_game_defs) look for next to the exe:
#   recomp_library.exe
#   games\<id>\game.toml            manifests of the games in -RtGames (facts only: ids, hashes, names); games\generic is the generic
#                                   runtime (experimental builds of titles without a port: the library writes their manifest from the dump)
#   tools\scripts\*.py              nup_extract.py, make_manifest.py, extract_generic.py, bnsh_programs.py
#   tools\spirv_harness\            the shader translator, self-contained (.NET runtime included)
#   tools\python\                   python.org's embeddable Python with zstandard, lz4 and capstone
#   tools\build\                    the player-side game build: game_build.py, the lifter (recomp\), Zig (zig\), nx_plain_nso.exe
#   games\<id>\sdk\                 per game with a runtime module: stub.cpp, rt_api.h, game.rc, game.manifest.in, <id>_rt.lib
#   games\<id>\runtime\             <id>_rt.dll, the ASTC decoder DLLs, fonts\ (copied next to the exe the player builds)
#   upscalers\streamline\          with -Streamline: sl.interposer.dll, sl.common.dll, sl.dlss.dll (MIT); nvngx_dlss.dll is never shipped (fetch_dlss.py)
#   LICENSE, LEGAL.md, README.md, THIRD_PARTY_NOTICES.md, licenses\
# No game data and no lifted code: the package holds this repository's own programs only (LEGAL.md). A game exe is never part of
# it: the library builds it on the player's PC from their own dump (tools\build\game_build.py).
#
#   powershell -ExecutionPolicy Bypass -File tools\release\package.ps1 -Version 0.1.0 [-Out work\release] [-RtGames generic]
#              [-RtBuildDir <existing runtime DLL build of the first game>] [-SkipTranslator] [-SkipPython] [-SkipRuntime]
# Needs, on PATH or in the environment: a VS x64 developer environment (rc.exe, the Windows SDK), clang-cl, cmake, ninja, the .NET 10
# SDK, and a Python 3.13 with pip (it fetches the wheels for the bundled Python). The Ryujinx submodule (refs\ryubing) must be checked out.
# The runtime DLL also needs the ASTC transcoder (tools\textures\build\release, tools\textures\scripts\build.ps1) and the vcpkg media
# libraries (dav1d, libvpx, opus: x64-windows-static; runtime/cmake/media_deps.cmake), or the movies are left out.
param(
    [string]$Version = 'dev',
    [string]$Out = '',
    [string[]]$RtGames = @('generic'),   # game ports are opt-in: -RtGames <id>,generic
    [string]$RtBuildDir = '',
    [switch]$SkipTranslator,
    [switch]$SkipPython,
    [switch]$SkipRuntime,
    [switch]$Streamline,     # opt-in: ship NVIDIA's MIT Streamline DLLs in upscalers\streamline\ (never nvngx_dlss.dll)
    [string]$HostPython = 'python'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Expand-Archive's progress bar costs minutes in Windows PowerShell
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Out) { $Out = Join-Path $repo 'work\release' }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$name = "recomp-library-$Version-win-x64"
$pkg = Join-Path $Out $name
$tmp = Join-Path $Out '_build'
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Force $pkg, $tmp | Out-Null

function Run([string]$exe, [string[]]$a) {
    Write-Host "> $exe $($a -join ' ')"
    & $exe @a
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# Pinned downloads (official sources, SHA-256 checked).
$PyVersion = '3.13.17'
$PyEmbedUrl = "https://www.python.org/ftp/python/$PyVersion/python-$PyVersion-embed-amd64.zip"
$PyEmbedSha256 = '9E35319BFFEDAB8B36AD44D8F377CB4F1733BCEC257A0B6D04D94F01486DDB38'
$PyWheels = @('zstandard', 'lz4', 'capstone==5.0.9')   # capstone: the lifter's decoder (recomp/requirements.txt)
$ZigVersion = '0.17.0'
$ZigUrl = "https://ziglang.org/download/$ZigVersion/zig-x86_64-windows-$ZigVersion.zip"
$ZigSha256 = 'B5663F69581DCF391293FBF16C06CB80D81D806545CE618B4D0BAB7F0EB8C428'   # ziglang.org/download/index.json

# Streamline v2.14.16 (MIT) release zip, used only with -Streamline. Asset name assumed from NVIDIA-RTX/Streamline's release page.
$SlUrl = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'
$SlSha256 = 'TODO-fill-on-first-run'   # not computed yet (no download was allowed when this was written): fill in, then the step runs

function Fetch([string]$url, [string]$file, [string]$sha) {
    if (-not (Test-Path $file) -or (Get-FileHash $file -Algorithm SHA256).Hash -ne $sha) {
        Write-Host "> download $url"
        & curl.exe -fsSL --retry 5 --retry-delay 3 -o $file $url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $url (curl exit $LASTEXITCODE)" }
    }
    $h = (Get-FileHash $file -Algorithm SHA256).Hash
    if ($h -ne $sha) { throw "$url : SHA-256 $h, expected $sha" }
}

# 1. recomp_library.exe
$lb = Join-Path $tmp 'library'
Run cmake @('-S', (Join-Path $repo 'tools\release\library'), '-B', $lb, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_COMPILER=clang-cl', "-DLIBRARY_VERSION=$Version")
Run cmake @('--build', $lb)
Copy-Item (Join-Path $lb 'recomp_library.exe') $pkg

# 2. game manifests: only the games this package carries a runtime for (-RtGames). A game without one could never be built or
#    played from the package; the library shows a dump of it as "No port yet".
foreach ($id in $RtGames) {
    $m = Join-Path $repo "recomp\games\$id\game.toml"
    if (-not (Test-Path $m)) { throw "no manifest for game '$id': $m" }
    $d = Join-Path $pkg "games\$id"
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item $m $d
}

# 3. the scripts the preparation steps run
$sd = Join-Path $pkg 'tools\scripts'
New-Item -ItemType Directory -Force $sd | Out-Null
Copy-Item (Join-Path $repo 'tools\extract\nup_extract.py') $sd
Copy-Item (Join-Path $repo 'tools\extract\make_manifest.py') $sd
Copy-Item (Join-Path $repo 'tools\shaders\scripts\extract_generic.py') $sd
Copy-Item (Join-Path $repo 'tools\shaders\scripts\bnsh_programs.py') $sd
Copy-Item (Join-Path $repo 'tools\release\fetch_dlss.py') $sd

# 3a. Streamline DLLs (MIT, opt-in)
if ($Streamline) {
    if ($SlSha256 -like 'TODO*') { throw 'package.ps1: $SlSha256 (Streamline v2.14.16 zip) is not filled in yet; download it once, check it, put its SHA-256 in the script' }
    $slzip = Join-Path $tmp 'streamline.zip'
    Fetch $SlUrl $slzip $SlSha256
    $slx = Join-Path $tmp 'streamline'
    if (Test-Path $slx) { Remove-Item -Recurse -Force $slx }
    Expand-Archive $slzip $slx
    $sld = Join-Path $pkg 'upscalers\streamline'
    New-Item -ItemType Directory -Force $sld | Out-Null
    foreach ($dll in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll') {
        $f = Get-ChildItem $slx -Recurse -Filter $dll | Where-Object { $_.FullName -match 'bin\\x64\\' -and $_.FullName -notmatch 'development' } | Select-Object -First 1
        if (-not $f) { throw "$dll not found in the Streamline zip" }
        Copy-Item $f.FullName $sld
    }
}

# 3b. the player-side game build: game_build.py, the lifter, the NSO converter, Zig
$bt = Join-Path $pkg 'tools\build'
New-Item -ItemType Directory -Force $bt | Out-Null
Copy-Item (Join-Path $repo 'game-build\release_b\game_build.py') $bt
$rc = Join-Path $bt 'recomp'
New-Item -ItemType Directory -Force $rc | Out-Null
Copy-Item (Join-Path $repo 'recomp\LICENSE'), (Join-Path $repo 'recomp\THIRD_PARTY.md') $rc
Copy-Item (Join-Path $repo 'recomp\include') $rc -Recurse
Copy-Item (Join-Path $repo 'recomp\nxrecomp') $rc -Recurse
Get-ChildItem $rc -Recurse -Directory -Filter '__pycache__' | Remove-Item -Recurse -Force
Get-ChildItem (Join-Path $repo 'recomp\games') -Directory | ForEach-Object {
    $d = Join-Path $rc "games\$($_.Name)"
    New-Item -ItemType Directory -Force $d | Out-Null
    Get-ChildItem $_.FullName -File | Where-Object { $_.Extension -in '.py', '.sites', '.toml' } | Copy-Item -Destination $d
}
$xb = Join-Path $tmp 'extract'
Run cmake @('-S', (Join-Path $repo 'tools\extract'), '-B', $xb, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl')
Run cmake @('--build', $xb, '--target', 'nx_plain_nso')
Copy-Item (Join-Path $xb 'bin\nx_plain_nso.exe') $bt
$zz = Join-Path $tmp "zig-$ZigVersion.zip"
Fetch $ZigUrl $zz $ZigSha256
$zx = Join-Path $tmp 'zig_x'
if (Test-Path $zx) { Remove-Item -Recurse -Force $zx }
Expand-Archive $zz $zx
Move-Item (Get-ChildItem $zx -Directory | Select-Object -First 1).FullName (Join-Path $bt 'zig')

# 3c. the runtime DLL of each game with a runtime module (runtime\games\<id>): the exe the player builds links its import library
if (-not $SkipRuntime) {
    foreach ($id in $RtGames) {
        $man = Join-Path $repo "recomp\games\$id\game.toml"
        $rtdll = ((Get-Content $man -Raw) | Select-String '(?m)^runtime_dll\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
        $stem = [IO.Path]::GetFileNameWithoutExtension($rtdll)
        $useRb = $RtBuildDir -and $id -eq $RtGames[0]     # an existing build folder holds one game's runtime
        $rb = if ($useRb) { $RtBuildDir } else { Join-Path $tmp "rt_$id" }
        if (-not $useRb -or -not (Test-Path (Join-Path $rb 'CMakeCache.txt'))) {
            Run cmake @('-S', (Join-Path $repo 'game-build'), '-B', $rb, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                        '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl', "-DGAME_VERSION=$Version",
                        "-DGAME_MANIFEST=$man", '-DGAME_RELEASE=ON', '-DGAME_BUILD_RT_DLL=ON')
        }
        Run cmake @('--build', $rb, '--target', 'game_rt')
        $gs = Join-Path $pkg "games\$id\sdk"
        $gr = Join-Path $pkg "games\$id\runtime"
        New-Item -ItemType Directory -Force $gs, (Join-Path $gr 'fonts') | Out-Null
        Copy-Item (Join-Path $repo 'game-build\src\stub.cpp'), (Join-Path $repo 'game-build\src\rt_api.h'),
                  (Join-Path $repo 'game-build\res\game.rc'), (Join-Path $repo 'game-build\res\game.manifest.in') $gs
        Copy-Item (Join-Path $rb "$stem.lib") $gs
        Copy-Item (Join-Path $rb "$stem.dll") $gr
        Get-ChildItem $rb -Filter 'astcdec-*-shared.dll' | Copy-Item -Destination $gr
        Copy-Item (Join-Path $repo 'runtime\assets\fonts\DejaVuSans.ttf') (Join-Path $gr 'fonts')
    }
}

# 4. the shader translator (Ryujinx's translator + this repository's patches), self-contained so players need no .NET
if (-not $SkipTranslator) {
    if (-not (Test-Path (Join-Path $repo 'refs\ryubing\src\Ryujinx.Graphics.Shader'))) {
        throw 'refs\ryubing is not checked out: git submodule update --init'
    }
    Run powershell @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'tools\shaders\scripts\setup_ryujinx.ps1'))
    Run dotnet @('publish', (Join-Path $repo 'tools\shaders\harness\SpirvHarness.csproj'), '-c', 'Release', '-r', 'win-x64',
                 '--self-contained', 'true', '-o', (Join-Path $pkg 'tools\spirv_harness'), "-p:PatchPython=$HostPython")
}

# 5. the bundled Python
if (-not $SkipPython) {
    $zip = Join-Path $tmp "python-$PyVersion-embed-amd64.zip"
    Fetch $PyEmbedUrl $zip $PyEmbedSha256
    $pd = Join-Path $pkg 'tools\python'
    Expand-Archive $zip $pd
    $site = Join-Path $pd 'Lib\site-packages'
    New-Item -ItemType Directory -Force $site | Out-Null
    # binary wheels for the embedded interpreter (cp313, win_amd64), whatever the host Python is
    Run $HostPython (@('-m', 'pip', 'install', '--disable-pip-version-check', '--no-compile', '--only-binary=:all:', '--platform', 'win_amd64',
                       '--python-version', '3.13', '--implementation', 'cp', '--target', $site) + $PyWheels)
    $pth = Get-ChildItem $pd -Filter 'python*._pth' | Select-Object -First 1
    if (-not $pth) { throw 'no ._pth file in the embeddable Python' }
    Add-Content -Path $pth.FullName -Value 'Lib\site-packages' -Encoding ascii
    $chk = & (Join-Path $pd 'python.exe') -I -c 'import zstandard, lz4.block, lz4.frame, capstone, tomllib, sys; print(sys.version.split()[0])'
    if ($LASTEXITCODE -ne 0) { throw 'the bundled Python cannot import zstandard / lz4 / capstone' }
    Write-Host "bundled Python $chk with $($PyWheels -join ', ')"
}

# 6. licences and notices
Copy-Item (Join-Path $repo 'LICENSE') $pkg
foreach ($f in 'LEGAL.md', 'README.md') { if (Test-Path (Join-Path $repo $f)) { Copy-Item (Join-Path $repo $f) $pkg } }
Copy-Item (Join-Path $repo 'runtime\THIRD_PARTY_NOTICES.md') $pkg
$ld = Join-Path $pkg 'licenses'
New-Item -ItemType Directory -Force $ld | Out-Null
$zl = Join-Path $pkg 'tools\build\zig\LICENSE'
if (Test-Path $zl) { Copy-Item $zl (Join-Path $ld 'LICENSE.zig.MIT.txt') }
if (Test-Path (Join-Path $repo 'tools\textures\licenses')) { Copy-Item (Join-Path $repo 'tools\textures\licenses\*') $ld -Recurse -Force }
if (Test-Path (Join-Path $repo 'tools\shaders\licenses')) { Copy-Item (Join-Path $repo 'tools\shaders\licenses\*') $ld -Recurse -Force }
if (-not $SkipPython) {
    $pl = Join-Path $pkg 'tools\python\LICENSE.txt'
    if (Test-Path $pl) { Copy-Item $pl (Join-Path $ld 'Python-PSF.txt') }
}
@"
Recomp Library $Version (Windows x64)

Start recomp_library.exe and add your own dump of a supported game (drag and drop, or Add): a .nup package or a folder
with the decrypted game files (exefs\ and romfs\). Play builds the game program from your dump on this PC (once, 15-40 minutes),
then prepares the shaders and starts the game.
This package contains no game files, no game code, no game program and no keys; you need your own legally dumped copy.
Bundled: the shader translator (Ryujinx's shader translator, MIT, with this project's patches; .NET runtime included),
python.org's embeddable Python $PyVersion (PSF licence) with zstandard, lz4 and capstone (BSD), Zig $ZigVersion (MIT; LLVM parts
Apache-2.0 WITH LLVM-exception) for the game build, the recompiler and runtime of this project (GPL-3.0), and the game manifests.
Source code: https://github.com/windowslucker1121/AiO-S2-Recompiler (GPL-3.0, see LICENSE).
"@ | Set-Content -Path (Join-Path $pkg 'README.txt') -Encoding utf8

# 7. zip + checksum
$zipOut = Join-Path $Out "$name.zip"
if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath $zipOut -CompressionLevel Optimal
$sha = (Get-FileHash $zipOut -Algorithm SHA256).Hash.ToLower()
"$sha  $name.zip" | Set-Content -Path "$zipOut.sha256" -Encoding ascii
Write-Host "package: $zipOut ($sha)"
$global:LASTEXITCODE = 0
