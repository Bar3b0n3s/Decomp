#!/usr/bin/env bash
# Regenerates the committed PE/COFF/PDB fixtures. Developer-only: needs clang-cl, lld-link,
# llvm-dlltool and llvm-lib (LLVM 18+); the tool itself never runs these.
set -euo pipefail
cd "$(dirname "$0")"

CLANG_CL=${CLANG_CL:-$(command -v clang-cl || echo /usr/lib/llvm-18/bin/clang-cl)}
CFLAGS=(/nologo /O2 /Gy /GS- /GR- /EHs-c- /Zl /Z7 /Brepro)
LDFLAGS=(/nologo /nodefaultlib /entry:entry /subsystem:console /debug /pdbaltpath:%_PDB% /pdbsourcepath:C:/fixtures /Brepro)

build_arch() {
    local arch=$1 target=$2
    rm -rf "$arch" && mkdir -p "$arch"
    for src in basic other; do
        "$CLANG_CL" --target="$target" "${CFLAGS[@]}" -c "src/$src.cpp" "/Fo$arch/$src.obj"
    done
    "$CLANG_CL" --target="$target" "${CFLAGS[@]}" -c candidates/mutated.cpp "/Fo$arch/mutated.obj"
    if [[ $arch == x86 ]]; then
        llvm-dlltool -m i386 -k -d src/kernel32_x86.def -l "$arch/kernel32.lib"
    else
        llvm-lib /nologo /machine:x64 /def:src/kernel32_x64.def "/out:$arch/kernel32.lib"
    fi
    lld-link "${LDFLAGS[@]}" "/out:$arch/basic.exe" "/pdb:$arch/basic.pdb" "$arch/basic.obj" "$arch/other.obj" "$arch/kernel32.lib"
    # Same program without base relocations (/FIXED), as many old EXEs ship.
    lld-link "${LDFLAGS[@]}" /fixed "/out:$arch/basic_fixed.exe" "/pdb:$arch/basic_fixed.pdb" "$arch/basic.obj" "$arch/other.obj" "$arch/kernel32.lib"
    rm -f "$arch/kernel32.lib" "$arch/basic_fixed.pdb" "$arch/basic.lib" "$arch/basic_fixed.lib"
}

build_arch x86 i686-pc-windows-msvc
build_arch x64 x86_64-pc-windows-msvc
echo "fixtures rebuilt"
