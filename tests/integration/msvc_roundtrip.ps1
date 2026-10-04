# Builds the fixture program with the real MSVC toolchain (cl.exe + link.exe from a developer
# environment), then checks that every function diffs byte-exact against the objects it was linked
# from. Exercises MSVC-specific output the clang-cl fixtures cannot: Rich headers, MSVC PDBs,
# x86 jump tables inside .text and x64 RVA jump tables.
param([ValidateSet("x86", "x64")][string]$Arch = "x64")
$ErrorActionPreference = "Stop"

$root = Resolve-Path "$PSScriptRoot\..\.."
$src = Join-Path $root "tests\fixtures\src"
$out = Join-Path $root "build\msvc-roundtrip-$Arch"
$decomp = Join-Path $root "bin\Release\decomp.exe"
New-Item -ItemType Directory -Force $out | Out-Null

$cflags = @("/nologo", "/c", "/O2", "/Gy", "/GS-", "/GR-", "/EHs-c-", "/Zl", "/Z7")
foreach ($name in "basic", "other") {
    & cl.exe @cflags "$src\$name.cpp" "/Fo$out\$name.obj"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on $name.cpp" }
}
& link.exe /nologo /nodefaultlib /entry:entry /subsystem:console /debug "/out:$out\basic.exe" "/pdb:$out\basic.pdb" `
    "$out\basic.obj" "$out\other.obj" kernel32.lib
if ($LASTEXITCODE -ne 0) { throw "link.exe failed" }

& $decomp info "$out\basic.exe"
if ($LASTEXITCODE -ne 0) { throw "decomp info failed" }

$failed = $false
foreach ($obj in "basic", "other") {
    & $decomp diff --binary "$out\basic.exe" --obj "$out\$obj.obj" --all
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) {
    foreach ($fn in "dispatch", "message", "scale", "entry") {
        & $decomp diff $fn --binary "$out\basic.exe" --obj "$out\basic.obj" --compact
    }
    throw "MSVC round trip ($Arch): some functions are not byte-exact"
}
Write-Host "MSVC round trip ($Arch): all functions byte-exact"
