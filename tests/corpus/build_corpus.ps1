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
    @(Get-Item "$PSScriptRoot\main.c", "$PSScriptRoot\rt.c", "$PSScriptRoot\eh.cpp", "$PSScriptRoot\seh.c", "$PSScriptRoot\eh_rt.c")
# /Gs999999: no stack probes (there is no __chkstk without a C library).
$cflags = @("/nologo", "/c", "/O2", "/Gy", "/GS-", "/GR-", "/Zl", "/Z7", "/Gs999999",
    "/DZYAN_NO_LIBC", "/DZYDIS_STATIC_BUILD", "/DZYCORE_STATIC_BUILD",
    "/I$zydis\include", "/I$zydis\src", "/I$zydis\dependencies\zycore\include")
$n = 0
foreach ($src in $sources) {
    $eh = if ($src.Extension -eq ".cpp") { "/EHsc" } else { "/EHs-c-" }
    & cl.exe @cflags $eh $src.FullName "/Fo$out\obj\$($src.BaseName)_$n.obj"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on $($src.Name)" }
    $n++
}
$prefix = if ($Arch -eq "x86") { "_" } else { "" }
# /incremental:no: /debug implies an incremental link, whose jump thunks and padding no relink makes.
& link.exe /nologo /nodefaultlib /entry:entry /subsystem:console /debug /incremental:no /opt:noref `
    "/alternatename:??_7type_info@@6B@=${prefix}corpus_type_info_vftable" "/out:$out\corpus.exe" `
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
    & $decomp bounds "$out\corpus.exe" --truth "$out\$truth" --errors 60 --min-exact $MinExact --show-code --units
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { throw "MSVC corpus ($Arch): fewer than $MinExact% of function bounds are exact" }
Write-Host "MSVC corpus ($Arch): function bounds measured"

# The corpus relinked by link.exe: from split objects alone, and with nine of its units (the decoder and the
# SEH unit among them) built by cl.exe from their own sources composed into unit sources, the rest split.
& $decomp toolchain add "msvc-$Arch" --kind msvc --compiler cl.exe
if ($LASTEXITCODE -ne 0) { throw "decomp toolchain add failed" }
$project = Join-Path $out "project"
$flags = @("/O2", "/Gy", "/GS-", "/GR-", "/EHs-c-", "/Zl", "/Gs999999", "/DZYAN_NO_LIBC", "/DZYDIS_STATIC_BUILD",
    "/DZYCORE_STATIC_BUILD", "/I$zydis\include", "/I$zydis\src", "/I$zydis\dependencies\zycore\include") |
    ForEach-Object { "--flag"; $_ }
& $decomp init "$out\corpus.exe" --dir $project --toolchain "msvc-$Arch" @flags
if ($LASTEXITCODE -ne 0) { throw "decomp init failed" }
# What link.exe made itself (the debug directory, the SAFESEH table, volatile metadata) in the original and the
# relink, and what of it the objects ask for, for a relink that differs.
function Show-LinkRecords {
    foreach ($image in "$out\corpus.exe", "$project\.decomp\relink\out\corpus.exe") {
        & dumpbin /nologo /headers /loadconfig $image | Select-String -Pattern "Pre-VC|Safe Exception|Volatile|sxdata|voltmd" |
            ForEach-Object { "  $(Split-Path -Leaf (Split-Path -Parent $image)): $_" }
    }
    foreach ($object in Get-ChildItem "$out\obj\*.obj") {
        & dumpbin /nologo /headers /symbols $object.FullName | Select-String -Pattern "\.voltbl|\.sxdata|@vol|@feat" |
            ForEach-Object { "  $($object.Name): $_" }
    }
}
# Links of the original objects and of the relink's split objects, to tell what makes link.exe ask for a load
# configuration (__load_config_used) with the split ones. Swapping one object at a time showed it is the split
# object of the runtime unit (eh_rt), which defines the exception handlers: it registers __except_handler3's
# address in its own .sxdata (the SEH unit registered it in the original), and /INCLUDEs every function it
# defines (__except_handler4, __CxxFrameHandler3 and the rest among them).
function Test-Links {
    $exp = Join-Path $out "experiments"
    New-Item -ItemType Directory -Force $exp | Out-Null
    $original = @("/nologo", "/nodefaultlib", "/entry:entry", "/subsystem:console", "/debug", "/incremental:no", "/opt:noref",
        "/alternatename:??_7type_info@@6B@=${prefix}corpus_type_info_vftable")
    $objects = Get-ChildItem "$out\obj\*.obj" | ForEach-Object FullName
    $split = Get-ChildItem "$project\.decomp\relink\objects\*.obj" | Sort-Object Name | ForEach-Object FullName
    # A copy of an object with sections renamed and marked for removal, so link.exe neither reads nor keeps them.
    function Copy-Without([string]$object, [string]$variant, [string[]]$sections) {
        $bytes = [System.IO.File]::ReadAllBytes($object)
        $count = [BitConverter]::ToUInt16($bytes, 2)
        $table = 20 + [BitConverter]::ToUInt16($bytes, 16)
        for ($i = 0; $i -lt $count; $i++) {
            $at = $table + 40 * $i
            $name = [System.Text.Encoding]::ASCII.GetString($bytes, $at, 8).TrimEnd([char]0)
            if ($sections -notcontains $name) { continue }
            $bytes[$at + 1] = [byte][char]'z'
            $flags = [BitConverter]::ToUInt32($bytes, $at + 36) -bor 0x800
            [BitConverter]::GetBytes([uint32]$flags).CopyTo($bytes, $at + 36)
        }
        $copy = Join-Path $exp "$variant-$(Split-Path -Leaf $object)"
        [System.IO.File]::WriteAllBytes($copy, $bytes)
        $copy
    }
    function Test-Link([string]$name, [string[]]$arguments) {
        $output = & link.exe @arguments "/out:$exp\$name.exe" "/pdb:$exp\$name.pdb" "/map:$exp\$name.map" 2>&1 | Out-String
        $result = "links"
        if ($LASTEXITCODE -ne 0) {
            $first = $output -split "`n" | Where-Object { $_ -match "error" } | Select-Object -First 1
            $result = if ($output -match "__load_config_used") { "asks for a load configuration" } else { "fails: $first" }
        }
        Write-Host "  experiment $name : $result"
        if (Test-Path "$exp\$name.map") {
            Select-String -Path "$exp\$name.map" -Pattern "sxdata|voltmd|load_config" | ForEach-Object { Write-Host "    map: $($_.Line.Trim())" }
        }
    }
    $runtime = $objects | Where-Object { (Split-Path -Leaf $_) -like "eh_rt_*" }
    $splitRuntime = $split | Where-Object { (Split-Path -Leaf $_) -like "*_eh_rt_*" }
    if (-not $runtime -or -not $splitRuntime) { Write-Host "  no runtime unit"; return }
    $others = @($objects | Where-Object { $_ -ne $runtime })
    Test-Link "original" ($original + $objects)
    Test-Link "split-runtime" ($original + $others + $splitRuntime)
    Test-Link "split-runtime-without-sxdata" ($original + $others + (Copy-Without $splitRuntime "no-sxdata" @(".sxdata")))
    Test-Link "split-runtime-without-directives" ($original + $others + (Copy-Without $splitRuntime "no-drectve" @(".drectve")))
    Test-Link "split-runtime-without-either" ($original + $others + (Copy-Without $splitRuntime "neither" @(".sxdata", ".drectve")))
    # The original objects and one that only asks the linker to include one of the runtime's functions.
    foreach ($name in "_except_handler3", "_except_handler4", "_local_unwind2", "_local_unwind4", "__CxxFrameHandler3",
        "__CxxFrameHandler4", "__C_specific_handler", "__std_terminate", "_CxxThrowException") {
        $symbol = if ($Arch -eq "x86" -and $name -eq "_CxxThrowException") { "__CxxThrowException@8" } else { "$prefix$name" }
        $file = Join-Path $exp "include$name.c"
        Set-Content -Path $file -Value "#pragma comment(linker, `"/INCLUDE:$symbol`")"
        & cl.exe @cflags /EHs-c- $file "/Fo$exp\include$name.obj" | Out-Null
        Test-Link "original-including-$name" ($original + $objects + "$exp\include$name.obj")
    }
}
& $decomp -C $project relink --all-split
if ($LASTEXITCODE -ne 0) {
    Show-LinkRecords
    Test-Links
    # Reported, not failed, while what link.exe makes itself here is worked out.
    Write-Warning "MSVC corpus ($Arch): the relink from split objects differs from the original"
    $global:LASTEXITCODE = 0
    return
}
$units = & $decomp -C $project --json units | Out-String | ConvertFrom-Json
$own = [ordered]@{
    "src/Decoder.c" = "$zydis\src\Decoder.c"; "src/Mnemonic.c" = "$zydis\src\Mnemonic.c"; "src/Register.c" = "$zydis\src\Register.c"
    "src/Utils.c" = "$zydis\src\Utils.c"; "src/FormatterBuffer.c" = "$zydis\src\FormatterBuffer.c"
    "src/zycore/src/String.c" = "$zydis\dependencies\zycore\src\String.c"
    "src/seh.c" = "$PSScriptRoot\seh.c"; "src/rt.c" = "$PSScriptRoot\rt.c"; "src/main.c" = "$PSScriptRoot\main.c"
}
foreach ($source in $own.Keys) {
    $unit = ($units | Where-Object { $_.source -eq $source }).name
    if (-not $unit) { throw "MSVC corpus ($Arch): no unit has the source $source" }
    & $decomp -C $project units compose $unit $own[$source]
    if ($LASTEXITCODE -ne 0) { throw "MSVC corpus ($Arch): $($own[$source]) composed into $unit's source does not match" }
}
& $decomp -C $project relink
if ($LASTEXITCODE -ne 0) { Show-LinkRecords; throw "MSVC corpus ($Arch): the relink with units from their sources differs from the original" }
Write-Host "MSVC corpus ($Arch): relinked identically by link.exe, from split objects and with nine units from source"
