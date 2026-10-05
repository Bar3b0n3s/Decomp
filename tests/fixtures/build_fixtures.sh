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
    # kernel32.lib stays: the integration test relinks the program with whatever lld-link is installed.
    rm -f "$arch/basic_fixed.pdb" "$arch/basic.lib" "$arch/basic_fixed.lib"
}

# Hand-written code layouts (src/idioms_<arch>.s) for function discovery and switch tables: an
# executable without a PDB, and its map file (every function f has a label f_end).
build_idioms() {
    local arch=$1 target=$2
    shift 2
    "$CLANG" --target="$target" -c "src/idioms_$arch.s" -o "$arch/idioms.obj"
    lld-link /nologo /nodefaultlib /entry:entry /subsystem:console /Brepro "$@" \
        "/out:$arch/idioms.exe" "/map:$arch/idioms.map" "$arch/idioms.obj" "$arch/kernel32.lib"
    rm -f "$arch/idioms.obj" "$arch/idioms.lib"
}

# C++ classes compiled with RTTI (src/rtti.cpp): the image and its PDB, whose names are the truth for
# the class names, vftables and RTTI structures found in the image alone.
build_rtti() {
    local arch=$1 target=$2 type_info=$3
    "$CLANG_CL" --target="$target" /nologo /O2 /Gy /GS- /GR /EHs-c- /Zl /Z7 /Brepro -c src/rtti.cpp "/Fo$arch/rtti.obj"
    lld-link "${LDFLAGS[@]}" "/alternatename:??_7type_info@@6B@=$type_info" "/out:$arch/rtti.exe" "/pdb:$arch/rtti.pdb" "$arch/rtti.obj"
    rm -f "$arch/rtti.obj" "$arch/rtti.lib"
}

# A static library (src/minilib) and a program linked with it (src/libuser.c), with its PDB as the
# truth for the library-matching tests.
build_minilib() {
    local arch=$1 target=$2
    local objs=()
    for src in src/minilib/*.c; do
        local obj="$arch/minilib_$(basename "$src" .c).obj"
        "$CLANG_CL" --target="$target" "${CFLAGS[@]}" -c "$src" "/Fo$obj"
        objs+=("$obj")
    done
    llvm-lib /nologo "/out:$arch/minilib.lib" "${objs[@]}"
    "$CLANG_CL" --target="$target" "${CFLAGS[@]}" -c src/libuser.c "/Fo$arch/libuser.obj"
    lld-link "${LDFLAGS[@]}" "/out:$arch/libuser.exe" "/pdb:$arch/libuser.pdb" "$arch/libuser.obj" "$arch/minilib.lib"
    rm -f "${objs[@]}" "$arch/libuser.obj" "$arch/libuser.lib"
}

# C++ exception handling and structured exception handling (the corpus's eh.cpp, seh.c and eh_rt.c):
# the image and its PDB, the truth for the bounds of functions with code only exceptions reach.
build_eh() {
    local arch=$1 target=$2 prefix=$3
    local objs=()
    for src in ../corpus/eh.cpp ../corpus/seh.c ../corpus/eh_rt.c src/eh_main.c; do
        local eh=/EHs-c-
        [[ $src == *.cpp ]] && eh=/EHsc
        local obj="$arch/eh_$(basename "${src%.*}").obj"
        "$CLANG_CL" --target="$target" /nologo /O2 /Gy /GS- /GR- "$eh" /Zl /Z7 /Brepro -c "$src" "/Fo$obj"
        objs+=("$obj")
    done
    lld-link "${LDFLAGS[@]}" "/alternatename:??_7type_info@@6B@=${prefix}corpus_type_info_vftable" "/out:$arch/eh.exe" "/pdb:$arch/eh.pdb" "${objs[@]}"
    mv "$arch/eh_eh.obj" "$arch/eh.obj"  # kept: the candidate for the diff tests
    rm -f "${objs[@]}" "$arch/eh.lib"
}

build_arch x86 i686-pc-windows-msvc
build_arch x64 x86_64-pc-windows-msvc
CLANG=${CLANG:-$(command -v clang || echo /usr/lib/llvm-18/bin/clang)}
build_idioms x86 i686-pc-windows-msvc /fixed /safeseh:no   # no relocations, as VC6 programs ship
build_idioms x64 x86_64-pc-windows-msvc
build_rtti x86 i686-pc-windows-msvc _rtti_type_info_vftable
build_rtti x64 x86_64-pc-windows-msvc rtti_type_info_vftable
build_minilib x86 i686-pc-windows-msvc
build_minilib x64 x86_64-pc-windows-msvc
build_eh x86 i686-pc-windows-msvc _
build_eh x64 x86_64-pc-windows-msvc ""
echo "fixtures rebuilt"
