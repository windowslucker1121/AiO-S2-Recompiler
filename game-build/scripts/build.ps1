# Configure + build the game exe into game-build\build\release (runtime sources are compiled in; the game libraries are imported).
# -Release builds the package flavour instead (GAME_RELEASE=ON: GUI program, ./exefs, no build paths in the files) into game-build\build\relpkg,
# together with the runtime launcher: that is the folder tools\release\make_release.py packs.
# The game comes from its manifest (-Manifest, env GAME_MANIFEST; default: the only recomp/games/<id>/game.toml next to the runtime).
# Environment overrides use the manifest id uppercased as prefix: <ID>_RUNTIME, <ID>_BUILD_DIR, <ID>_LIB_DIR (as -Runtime, -Out, -LibDir).
param([string]$Config = "Release", [switch]$Clean, [switch]$Release, [string]$Manifest = $env:GAME_MANIFEST, [string]$Runtime, [string]$Out, [string]$Version = "dev", [string]$LibDir)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Runtime) { $Runtime = $env:GAME_RUNTIME }
if (-not $Runtime) {   # no -Runtime: the one <ID>_RUNTIME variable that points at a runtime with recomp/games next to it
    $c = @(Get-ChildItem env: | Where-Object { $_.Name -match '^[A-Z0-9]+_RUNTIME$' -and $_.Value -and (Test-Path (Join-Path $_.Value "..\recomp\games")) })
    if ($c.Count -eq 1) { $Runtime = $c[0].Value }
}
if (-not $Manifest) {
    $rt = if ($Runtime) { $Runtime } else { Join-Path $root "..\runtime" }
    $found = @(Get-ChildItem (Join-Path $rt "..\recomp\games\*\game.toml"))
    if ($found.Count -ne 1) { throw "set -Manifest (or GAME_MANIFEST): $($found.Count) game manifests found next to the runtime" }
    $Manifest = $found[0].FullName
}
$id = ((Get-Content $Manifest -Raw) | Select-String '(?ms)^\[game\].*?^id\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
$pfx = $id.ToUpper()
if (-not $Runtime) { $Runtime = [Environment]::GetEnvironmentVariable("${pfx}_RUNTIME") }
if (-not $Out) { $Out = [Environment]::GetEnvironmentVariable("${pfx}_BUILD_DIR") }
if (-not $LibDir) { $LibDir = [Environment]::GetEnvironmentVariable("${pfx}_LIB_DIR") }
$bd = if ($Out) { $Out } elseif ($Release) { Join-Path $root "build\relpkg" } else { Join-Path $root ("build\" + $Config.ToLower()) }
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
$env_cmd = Join-Path $root "..\runtime\scripts\env.cmd"
$cfg = "cmake -S `"$root`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DGAME_VERSION=$Version -DGAME_MANIFEST=`"$Manifest`""
if ($Runtime) { $cfg += " -DGAME_RUNTIME_DIR=`"$Runtime`"" }
if ($LibDir) { $cfg += " -DGAME_LIB_DIR=`"$LibDir`"" }
$targets = "game_exe"
if ($Release) { $cfg += " -DGAME_RELEASE=ON"; $targets = "game_exe ${id}_launcher" }
cmd /c "call `"$env_cmd`" && $cfg && cmake --build `"$bd`" --target $targets"
exit $LASTEXITCODE
