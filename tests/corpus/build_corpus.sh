#!/usr/bin/env bash
# Builds the function-bounds corpus: Zydis and Zycore (external/zydis, MIT) compiled without a C library
# (rt.c supplies memset, memcpy and the x86 64-bit division helper), with C++ exception handling
# (eh.cpp) and structured exception handling (seh.c; eh_rt.c supplies their runtime functions), into an
# executable per architecture, with its PDB (the ground truth for function bounds) and a link.exe-style
# map file.
#
#   tests/corpus/build_corpus.sh <out_dir>     -> <out_dir>/{x86,x64}/corpus.{exe,pdb,map}
#
# Needs clang-cl and lld-link (LLVM 18+). Unreferenced functions are kept (/opt:noref), so the analysis
# has to find code that nothing calls.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:?usage: build_corpus.sh <out_dir>}
CLANG_CL=${CLANG_CL:-$(command -v clang-cl || echo /usr/lib/llvm-18/bin/clang-cl)}
LLD_LINK=${LLD_LINK:-$(command -v lld-link || echo /usr/lib/llvm-18/bin/lld-link)}
zydis="$root/external/zydis"
sources=("$zydis"/src/*.c "$zydis"/dependencies/zycore/src/*.c "$here/main.c" "$here/rt.c" "$here/eh.cpp" "$here/seh.c" "$here/eh_rt.c")

build_arch() {
    local arch=$1 target=$2 prefix=$3
    local dir="$out/$arch"
    rm -rf "$dir" && mkdir -p "$dir/obj"
    local n=0
    for src in "${sources[@]}"; do
        local eh=/EHs-c-
        [[ $src == *.cpp ]] && eh=/EHsc
        "$CLANG_CL" --target="$target" /nologo /O2 /Gy /GS- /GR- "$eh" /Zl /Z7 /Brepro \
            -DZYAN_NO_LIBC -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD \
            "-I$zydis/include" "-I$zydis/src" "-I$zydis/dependencies/zycore/include" \
            -c "$src" "/Fo$dir/obj/$(basename "${src%.*}")_$n.obj"
        n=$((n + 1))
    done
    "$LLD_LINK" /nologo /nodefaultlib /entry:entry /subsystem:console /debug /opt:noref /Brepro \
        "/alternatename:??_7type_info@@6B@=${prefix}corpus_type_info_vftable" \
        "/out:$dir/corpus.exe" "/pdb:$dir/corpus.pdb" "/map:$dir/corpus.map" "$dir"/obj/*.obj
    rm -rf "$dir/obj"
}

build_arch x86 i686-pc-windows-msvc _
build_arch x64 x86_64-pc-windows-msvc ""
echo "corpus built in $out"
