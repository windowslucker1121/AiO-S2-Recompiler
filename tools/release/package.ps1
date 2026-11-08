# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the game library release package (recomp_library.exe and everything it runs) into a folder and a zip.
# The layout is the one runtime/apps/setup/setup_core.cpp (find_tools) and setup_games.cpp (find_game_defs) look for next to the exe:
#   recomp_library.exe
#   games\<id>\game.toml            manifests of every game in recomp\games (facts only: ids, hashes, names)
#   tools\scripts\*.py              nup_extract.py, extract_generic.py, bnsh_programs.py
#   tools\spirv_harness\            the shader translator, self-contained (.NET runtime included)
#   tools\python\                   python.org's embeddable Python with zstandard and lz4
#   LICENSE, LEGAL.md, README.md, THIRD_PARTY_NOTICES.md, licenses\
# No game data and no lifted code: the package holds this repository's own programs only (LEGAL.md). A game exe is not part of it.
#
#   powershell -ExecutionPolicy Bypass -File tools\release\package.ps1 -Version 0.1.0 [-Out work\release] [-SkipTranslator]
# Needs, on PATH or in the environment: a VS x64 developer environment (rc.exe, the Windows SDK), clang-cl, cmake, ninja, the .NET 10
# SDK, and a Python 3.13 with pip (it fetches the wheels for the bundled Python). The Ryujinx submodule (refs\ryubing) must be checked out.
param(
    [string]$Version = 'dev',
    [string]$Out = '',
    [switch]$SkipTranslator,
    [switch]$SkipPython,
    [string]$HostPython = 'python'
)
$ErrorActionPreference = 'Stop'
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
$PyWheels = @('zstandard', 'lz4')

# 1. recomp_library.exe
$lb = Join-Path $tmp 'library'
Run cmake @('-S', (Join-Path $repo 'tools\release\library'), '-B', $lb, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_COMPILER=clang-cl', "-DLIBRARY_VERSION=$Version")
Run cmake @('--build', $lb)
Copy-Item (Join-Path $lb 'recomp_library.exe') $pkg

# 2. game manifests
Get-ChildItem (Join-Path $repo 'recomp\games') -Directory | ForEach-Object {
    $m = Join-Path $_.FullName 'game.toml'
    if (Test-Path $m) {
        $d = Join-Path $pkg "games\$($_.Name)"
        New-Item -ItemType Directory -Force $d | Out-Null
        Copy-Item $m $d
    }
}

# 3. the scripts the preparation steps run
$sd = Join-Path $pkg 'tools\scripts'
New-Item -ItemType Directory -Force $sd | Out-Null
Copy-Item (Join-Path $repo 'tools\extract\nup_extract.py') $sd
Copy-Item (Join-Path $repo 'tools\shaders\scripts\extract_generic.py') $sd
Copy-Item (Join-Path $repo 'tools\shaders\scripts\bnsh_programs.py') $sd

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
    if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $PyEmbedSha256) {
        Write-Host "> download $PyEmbedUrl"
        Invoke-WebRequest $PyEmbedUrl -OutFile $zip -UseBasicParsing
    }
    $h = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($h -ne $PyEmbedSha256) { throw "python embed zip: SHA-256 $h, expected $PyEmbedSha256" }
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
    $chk = & (Join-Path $pd 'python.exe') -I -c 'import zstandard, lz4.block, lz4.frame, sys; print(sys.version.split()[0])'
    if ($LASTEXITCODE -ne 0) { throw 'the bundled Python cannot import zstandard / lz4' }
    Write-Host "bundled Python $chk with $($PyWheels -join ', ')"
}

# 6. licences and notices
Copy-Item (Join-Path $repo 'LICENSE') $pkg
foreach ($f in 'LEGAL.md', 'README.md') { if (Test-Path (Join-Path $repo $f)) { Copy-Item (Join-Path $repo $f) $pkg } }
Copy-Item (Join-Path $repo 'runtime\THIRD_PARTY_NOTICES.md') $pkg
$ld = Join-Path $pkg 'licenses'
New-Item -ItemType Directory -Force $ld | Out-Null
if (Test-Path (Join-Path $repo 'tools\shaders\licenses')) { Copy-Item (Join-Path $repo 'tools\shaders\licenses\*') $ld -Recurse -Force }
if (-not $SkipPython) {
    $pl = Join-Path $pkg 'tools\python\LICENSE.txt'
    if (Test-Path $pl) { Copy-Item $pl (Join-Path $ld 'Python-PSF.txt') }
}
@"
Recomp Library $Version (Windows x64)

Start recomp_library.exe and add your own dump of a supported game (drag and drop, or Add).
This package contains no game files, no game code and no keys; you need your own legally dumped copy.
Bundled: the shader translator (Ryujinx's shader translator, MIT, with this project's patches; .NET runtime included),
python.org's embeddable Python $PyVersion (PSF licence) with zstandard and lz4 (BSD), and the manifests of the games in games\.
Source code: https://github.com/windowslucker1121/AiO-Recompiler (GPL-3.0, see LICENSE).
"@ | Set-Content -Path (Join-Path $pkg 'README.txt') -Encoding utf8

# 7. zip + checksum
$zipOut = Join-Path $Out "$name.zip"
if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath $zipOut -CompressionLevel Optimal
$sha = (Get-FileHash $zipOut -Algorithm SHA256).Hash.ToLower()
"$sha  $name.zip" | Set-Content -Path "$zipOut.sha256" -Encoding ascii
Write-Host "package: $zipOut ($sha)"
$global:LASTEXITCODE = 0
