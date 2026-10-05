# Builds the function-bounds corpus with the real MSVC toolchain (cl.exe + link.exe from a developer
# environment for -Arch), checks that the image's Rich header identifies cl.exe's own version, and
# measures the bounds Decomp finds without the PDB against the PDB and the map file. See
# build_corpus.sh for the corpus itself.
#
#   tests\corpus\build_corpus.ps1 -Arch x86 [-MinExact 95]
param([ValidateSet("x86", "x64")][string]$Arch = "x64", [double]$MinExact = 95)
$ErrorActionPreference = "Stop"

$root = Resolve-Path "$PSScriptRoot\..\.."
$zydis = Join-Path $root "external\zydis"
$out = Join-Path $root "build\corpus-msvc-$Arch"
$decomp = Join-Path $root "bin\Release\decomp.exe"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\obj" | Out-Null

$sources = @(Get-ChildItem "$zydis\src\*.c") + @(Get-ChildItem "$zydis\dependencies\zycore\src\*.c") +
    @(Get-Item "$PSScriptRoot\main.c") + @(Get-Item "$PSScriptRoot\rt.c")
# /Gs999999: no stack probes (there is no __chkstk without a C library).
$cflags = @("/nologo", "/c", "/O2", "/Gy", "/GS-", "/GR-", "/Zl", "/Z7", "/Gs999999",
    "/DZYAN_NO_LIBC", "/DZYDIS_STATIC_BUILD", "/DZYCORE_STATIC_BUILD",
    "/I$zydis\include", "/I$zydis\src", "/I$zydis\dependencies\zycore\include")
$n = 0
foreach ($src in $sources) {
    & cl.exe @cflags $src.FullName "/Fo$out\obj\$($src.BaseName)_$n.obj"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on $($src.Name)" }
    $n++
}
& link.exe /nologo /nodefaultlib /entry:entry /subsystem:console /debug /opt:noref "/out:$out\corpus.exe" `
    "/pdb:$out\corpus.pdb" "/map:$out\corpus.map" (Get-ChildItem "$out\obj\*.obj" | ForEach-Object FullName)
if ($LASTEXITCODE -ne 0) { throw "link.exe failed" }

& $decomp info "$out\corpus.exe"
if ($LASTEXITCODE -ne 0) { throw "decomp info failed" }

# The Rich header names the compiler that built the corpus: the version cl.exe itself reports.
$banner = cmd /c "cl.exe 2>&1" | Out-String
if ($banner -notmatch 'Version (\d+\.\d+\.\d+)') { throw "cannot read cl.exe's version from: $banner" }
$clVersion = $Matches[1]
$info = & $decomp --json info "$out\corpus.exe" | Out-String | ConvertFrom-Json
$main = $info.build.main_compiler
if (-not $main) { throw "the corpus's Rich header names no compiler" }
Write-Host "cl.exe $clVersion; the Rich header says $($main.description) ($($main.visual_studio))"
if ($main.version -ne $clVersion) { throw "the Rich header's compiler version $($main.version) is not cl.exe's $clVersion" }
if (-not $info.build.checksum_ok) { throw "the corpus's Rich header checksum does not match" }

$failed = $false
foreach ($truth in "corpus.pdb", "corpus.map") {
    & $decomp bounds "$out\corpus.exe" --truth "$out\$truth" --errors 60 --min-exact $MinExact --show-code
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { throw "MSVC corpus ($Arch): fewer than $MinExact% of function bounds are exact" }
Write-Host "MSVC corpus ($Arch): function bounds measured"
