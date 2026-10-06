# Builds the fixture program with the real MSVC toolchain (cl.exe + link.exe from a developer
# environment), then checks that every function diffs byte-exact against the objects it was linked
# from. Exercises MSVC-specific output the clang-cl fixtures cannot: Rich headers, MSVC PDBs,
# x86 jump tables inside .text and x64 RVA jump tables, and the type records cl.exe writes with /Z7.
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
    & $decomp diff other_value --binary "$out\basic.exe" --obj "$out\other.obj" --compact
    throw "MSVC round trip ($Arch): some functions are not byte-exact"
}
Write-Host "MSVC round trip ($Arch): all functions byte-exact"

# The agent end to end without an API key: a scripted replay matches add() with the real cl.exe.
$env:DECOMP_TOOLCHAINS = Join-Path $out "toolchains.json"
& $decomp toolchain add "msvc-$Arch" --kind msvc --compiler cl.exe
if ($LASTEXITCODE -ne 0) { throw "decomp toolchain add failed" }
$project = Join-Path $out "project"
if (Test-Path $project) { Remove-Item -Recurse -Force $project }
& $decomp init "$out\basic.exe" --dir $project --toolchain "msvc-$Arch" --flag /O2 --flag /Gy --flag /GS- --flag /GR- --flag /EHs-c-
if ($LASTEXITCODE -ne 0) { throw "decomp init failed" }
& $decomp -C $project agent add --replay "$root\tests\replay\agent_match_add.jsonl"
if ($LASTEXITCODE -ne 0) { throw "scripted agent run did not match add() with cl.exe ($Arch)" }
& $decomp -C $project status
# The PDB gives add its unit: the match went into the unit's source, which verifies with cl.exe.
& $decomp -C $project units verify
if ($LASTEXITCODE -ne 0) { throw "the unit source of add() does not verify with cl.exe ($Arch)" }
Write-Host "MSVC agent replay ($Arch): matched"

# The program linked again by link.exe, from split objects of its original bytes, then from its units'
# sources (composed from the fixture's own translation units and compiled by cl.exe): identical to the
# original once its build timestamps and PDB GUID are taken over.
& $decomp -C $project relink --all-split
if ($LASTEXITCODE -ne 0) { throw "the relink from split objects differs from the original with link.exe ($Arch)" }
& $decomp -C $project units compose basic.obj "$src\basic.cpp"
if ($LASTEXITCODE -ne 0) { throw "basic.cpp composed into basic.obj's source does not match with cl.exe ($Arch)" }
& $decomp -C $project units compose other.obj "$src\other.cpp"
if ($LASTEXITCODE -ne 0) { throw "other.cpp composed into other.obj's source does not match with cl.exe ($Arch)" }
& $decomp -C $project relink
if ($LASTEXITCODE -ne 0) { throw "the relink from the units' sources differs from the original with link.exe ($Arch)" }
Write-Host "MSVC relink ($Arch): identical to the original"

# The fixtures' types, declared in project headers (tests/fixtures/include), compile with cl.exe /Z7 to
# the layouts in the PDBs link.exe wrote: a struct, and classes with virtual functions, multiple and
# virtual inheritance.
Copy-Item "$root\tests\fixtures\include\basic.h" (Join-Path $project "include")
& $decomp -C $project types check
if ($LASTEXITCODE -ne 0) { throw "the types of include\basic.h differ from the PDB's with cl.exe ($Arch)" }
$typeInfo = if ($Arch -eq "x86") { "_rtti_type_info_vftable" } else { "rtti_type_info_vftable" }
& cl.exe /nologo /c /O2 /Gy /GS- /GR /EHs-c- /Zl /Z7 "$src\rtti.cpp" "/Fo$out\rtti.obj"
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on rtti.cpp" }
& link.exe /nologo /nodefaultlib /entry:entry /subsystem:console /debug "/alternatename:??_7type_info@@6B@=$typeInfo" `
    "/out:$out\rtti.exe" "/pdb:$out\rtti.pdb" "$out\rtti.obj"
if ($LASTEXITCODE -ne 0) { throw "link.exe failed on rtti.obj" }
$rttiProject = Join-Path $out "rtti-project"
if (Test-Path $rttiProject) { Remove-Item -Recurse -Force $rttiProject }
& $decomp init "$out\rtti.exe" --dir $rttiProject --toolchain "msvc-$Arch" --flag /O2 --flag /GR
if ($LASTEXITCODE -ne 0) { throw "decomp init failed (rtti)" }
Copy-Item "$root\tests\fixtures\include\rtti.h" (Join-Path $rttiProject "include")
& $decomp -C $rttiProject types check
if ($LASTEXITCODE -ne 0) { throw "the classes of include\rtti.h differ from the PDB's with cl.exe ($Arch)" }
Write-Host "MSVC types ($Arch): the headers' layouts equal the PDBs'"
# Class skeletons from the RTTI cl.exe wrote compile with cl.exe to the vtables and base offsets it says.
$skeletonProject = Join-Path $out "rtti-skeletons"
if (Test-Path $skeletonProject) { Remove-Item -Recurse -Force $skeletonProject }
& $decomp init "$out\rtti.exe" --dir $skeletonProject --toolchain "msvc-$Arch" --flag /O2 --flag /GR
if ($LASTEXITCODE -ne 0) { throw "decomp init failed (rtti skeletons)" }
& $decomp -C $skeletonProject types skeletons
if ($LASTEXITCODE -ne 0) { throw "the RTTI class skeletons do not compile to the RTTI's layout with cl.exe ($Arch)" }
Write-Host "MSVC class skeletons ($Arch): they compile to the RTTI's vtables and base offsets"

# Types whose layouts a header has to get right (packing, alignment, bitfields, anonymous unions, member
# pointers, virtual functions, multiple and virtual inheritance), built with cl.exe: declared from
# link.exe's PDB by decomp types import and compiled again with cl.exe, they have the PDB's layouts.
& cl.exe @cflags "$src\layouts.cpp" "/Fo$out\layouts.obj"
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on layouts.cpp" }
& link.exe /nologo /nodefaultlib /entry:entry /subsystem:console /debug "/out:$out\layouts.exe" "/pdb:$out\layouts.pdb" "$out\layouts.obj" kernel32.lib
if ($LASTEXITCODE -ne 0) { throw "link.exe failed on layouts.obj" }
$layoutsProject = Join-Path $out "layouts-project"
if (Test-Path $layoutsProject) { Remove-Item -Recurse -Force $layoutsProject }
& $decomp init "$out\layouts.exe" --dir $layoutsProject --toolchain "msvc-$Arch" --flag /O2 --flag /GR- --flag /EHs-c-
if ($LASTEXITCODE -ne 0) { throw "decomp init failed (layouts)" }
& $decomp -C $layoutsProject types import --all
if ($LASTEXITCODE -ne 0) { throw "decomp types import failed with cl.exe ($Arch)" }
& $decomp -C $layoutsProject types check
if ($LASTEXITCODE -ne 0) { throw "the imported types differ from the PDB's with cl.exe ($Arch)" }
Write-Host "MSVC type import ($Arch): the declarations compile to the PDB's layouts"
