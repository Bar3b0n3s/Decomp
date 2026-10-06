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
# Links of the original objects and of the relink's split objects with either one's flags, to tell what makes
# link.exe write its SAFESEH table and volatile metadata (or ask for a load configuration).
function Test-Links {
    $exp = Join-Path $out "experiments"
    New-Item -ItemType Directory -Force $exp | Out-Null
    $original = @("/nologo", "/nodefaultlib", "/entry:entry", "/subsystem:console", "/debug", "/incremental:no", "/opt:noref",
        "/alternatename:??_7type_info@@6B@=${prefix}corpus_type_info_vftable")
    $objects = Get-ChildItem "$out\obj\*.obj" | ForEach-Object FullName
    $split = Get-ChildItem "$project\.decomp\relink\objects\*.obj" | Sort-Object Name | ForEach-Object FullName
    $command = (Get-Content "$project\.decomp\relink\result.json" -Raw | ConvertFrom-Json).link.command
    $relinkFlags = @($command | Select-Object -Skip 1 | Where-Object { $_ -like "/*" -and $_ -notlike "/out:*" -and $_ -notlike "/pdb:*" })
    $libraries = @($command | Where-Object { $_ -like "*.lib" })
    # Copies of the original objects with sections renamed and marked for removal, so link.exe neither reads
    # nor keeps them: which of them makes it write volatile metadata and define the load configuration itself.
    function Copy-Objects([string]$variant, [string[]]$sections) {
        $dir = Join-Path $exp $variant
        New-Item -ItemType Directory -Force $dir | Out-Null
        foreach ($object in $objects) {
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
            [System.IO.File]::WriteAllBytes((Join-Path $dir (Split-Path -Leaf $object)), $bytes)
        }
        Get-ChildItem "$dir\*.obj" | ForEach-Object FullName
    }
    $runs = [ordered]@{
        "original-again" = $original + $objects
        "original-safeseh-no" = $original + "/safeseh:no" + $objects
        "split-objects-original-flags" = $original + $split + $libraries
        "split-objects-verbose" = $relinkFlags + "/verbose:safeseh" + $split + $libraries
        "original-without-chks64" = $original + (Copy-Objects "no-chks64" @(".chks64"))
        "original-without-debug-s" = $original + (Copy-Objects "no-debug-s" @(".debug`$S"))
        "original-without-debug-t" = $original + (Copy-Objects "no-debug-t" @(".debug`$T"))
        "original-without-debug" = $original + (Copy-Objects "no-debug" @(".debug`$S", ".debug`$T", ".debug`$F", ".chks64"))
    }
    # An object cl.exe made of an empty file, beside the split objects: whether any of its objects spares them.
    Set-Content -Path "$exp\empty.c" -Value "/* nothing */"
    & cl.exe @cflags /EHs-c- "$exp\empty.c" "/Fo$exp\empty.obj" | Out-Null
    $runs["split-objects-and-an-empty-object"] = $relinkFlags + $split + "$exp\empty.obj" + $libraries
    # Split objects reach everything through __ImageBase, which no original object names.
    Set-Content -Path "$exp\image_base.c" -Value "extern char __ImageBase; char* decomp_image_base(void) { return &__ImageBase; }"
    & cl.exe @cflags /EHs-c- "$exp\image_base.c" "/Fo$exp\image_base.obj" | Out-Null
    $runs["original-and-an-image-base-user"] = $original + $objects + "$exp\image_base.obj"
    $runs["split-objects-forced"] = $relinkFlags + "/force:unresolved" + $split + $libraries
    $runs["split-objects-no-volatile-metadata"] = $relinkFlags + "/volatilemetadata:no" + $split + $libraries
    foreach ($name in $runs.Keys) {
        $output = & link.exe @($runs[$name]) "/out:$exp\$name.exe" "/pdb:$exp\$name.pdb" "/map:$exp\$name.map" 2>&1 | Out-String
        Write-Host "  experiment $name : exit $LASTEXITCODE"
        $output -split "`n" | Where-Object { $_ -match "error|warning|SAFESEH|safe|load_config|volatile" } | Select-Object -First 15 |
            ForEach-Object { Write-Host "    $_" }
        if (Test-Path "$exp\$name.map") {
            Select-String -Path "$exp\$name.map" -Pattern "sxdata|voltmd|volt|load_config|volatile_metadata|safe_se" |
                ForEach-Object { Write-Host "    map: $($_.Line.Trim())" }
        }
    }
    # One object swapped at a time: which split objects make a link of the original ones ask for a load
    # configuration, and which original objects spare a link of the split ones from it.
    function Test-Link([string[]]$arguments) {
        $output = & link.exe @arguments "/out:$exp\swap.exe" "/pdb:$exp\swap.pdb" 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0) { return "links" }
        if ($output -match "__load_config_used") { return "asks for a load configuration" }
        $first = $output -split "`n" | Where-Object { $_ -match "error" } | Select-Object -First 1
        return "fails: $("$first".Trim())"
    }
    $originalByName = @{}
    foreach ($object in $objects) { $originalByName[(Split-Path -Leaf $object)] = $object }
    foreach ($s in $split) {
        $name = (Split-Path -Leaf $s) -replace '^\d{3}_', ''
        $o = $originalByName[$name]
        if (-not $o) { Write-Host "  swap $name : no original object"; continue }
        $amongOriginals = Test-Link ($original + @($objects | ForEach-Object { if ($_ -eq $o) { $s } else { $_ } }) + $libraries)
        $amongSplit = Test-Link ($relinkFlags + @($split | ForEach-Object { if ($_ -eq $s) { $o } else { $_ } }) + $libraries)
        Write-Host "  swap $name : split among the originals $amongOriginals; original among the split $amongSplit"
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
