# Compiler identification (decomp search identify) on one program built by three toolchains: cl.exe with
# link.exe, clang-cl with lld-link, and MinGW-w64 GCC with lld-link. For each build, the toolchain that made
# it ranks first, ahead of the other two, with every function byte-exact at its best flags. Runs in an
# x64 developer environment (cl.exe on the PATH).
$ErrorActionPreference = "Stop"

$root = Resolve-Path "$PSScriptRoot\..\.."
$src = Join-Path $root "tests\fixtures\src\ident.c"
$out = Join-Path $root "build\identify-x64"
$decomp = Join-Path $root "bin\Release\decomp.exe"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

function Find-Tool([string[]]$names, [string[]]$paths) {
    foreach ($n in $names) {
        $c = Get-Command $n -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($c) { return $c.Source }
    }
    foreach ($p in $paths) { if (Test-Path $p) { return $p } }
    return $null
}
$clang = Find-Tool @("clang-cl.exe") @("C:\Program Files\LLVM\bin\clang-cl.exe")
$lld = Find-Tool @("lld-link.exe") @("C:\Program Files\LLVM\bin\lld-link.exe")
$gcc = Find-Tool @("x86_64-w64-mingw32-gcc.exe") @("C:\mingw64\bin\x86_64-w64-mingw32-gcc.exe", "C:\mingw64\bin\gcc.exe", "C:\msys64\mingw64\bin\gcc.exe")
if (-not $clang -or -not $lld) { throw "clang-cl and lld-link are needed" }
if (-not $gcc) { throw "MinGW-w64 GCC is needed (x86_64-w64-mingw32-gcc.exe or C:\mingw64\bin\gcc.exe)" }
Write-Host "cl.exe: $((Get-Command cl.exe).Source)"
Write-Host "clang-cl: $clang"
Write-Host "gcc: $gcc"

# The candidates, in a registry of their own (clang-cl-x64 is the auto-detected one).
$env:DECOMP_TOOLCHAINS = Join-Path $out "toolchains.json"
& $decomp toolchain add msvc-x64 --kind msvc --compiler cl.exe
if ($LASTEXITCODE -ne 0) { throw "decomp toolchain add failed (msvc)" }
& $decomp toolchain add gcc-x64 --kind gcc --compiler $gcc
if ($LASTEXITCODE -ne 0) { throw "decomp toolchain add failed (gcc)" }
& $decomp toolchain list

# The program, three times.
& cl.exe /nologo /c /O2 /GS- /Zl /Z7 $src "/Fo$out\msvc.obj"
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed" }
& link.exe /nologo /nodefaultlib /entry:entry /subsystem:console /debug /incremental:no "/out:$out\msvc.exe" "/pdb:$out\msvc.pdb" "$out\msvc.obj"
if ($LASTEXITCODE -ne 0) { throw "link.exe failed" }
& $clang --target=x86_64-pc-windows-msvc /nologo /c /O2 /GS- /Zl /Z7 /Brepro $src "/Fo$out\clang.obj"
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed" }
& $lld /nologo /nodefaultlib /entry:entry /subsystem:console /debug /Brepro "/out:$out\clang.exe" "/pdb:$out\clang.pdb" "$out\clang.obj"
if ($LASTEXITCODE -ne 0) { throw "lld-link failed (clang-cl)" }
& $gcc -c -O2 $src -o "$out\gcc.o"
if ($LASTEXITCODE -ne 0) { throw "gcc failed" }
& $lld /nologo /nodefaultlib /entry:entry /subsystem:console /debug /Brepro "/out:$out\gcc.exe" "/pdb:$out\gcc.pdb" "$out\gcc.o"
if ($LASTEXITCODE -ne 0) { throw "lld-link failed (gcc)" }

$failed = @()
foreach ($build in @(@{ exe = "msvc"; toolchain = "msvc-x64" }, @{ exe = "clang"; toolchain = "clang-cl-x64" }, @{ exe = "gcc"; toolchain = "gcc-x64" })) {
    $json = Join-Path $out "$($build.exe).json"
    & $decomp --json search identify --binary "$out\$($build.exe).exe" --source $src --candidates "msvc-x64,clang-cl-x64,gcc-x64" |
        Set-Content $json
    & python "$root\tests\integration\search_checks.py" identify $json $build.toolchain
    if ($LASTEXITCODE -ne 0) {
        Get-Content $json
        $failed += $build.toolchain
    }
}
if ($failed.Count -gt 0) { throw "compiler identification did not rank the toolchain that built the program first: $($failed -join ', ')" }
Write-Host "Compiler identification: each of three toolchains ranks first on the program it built"
