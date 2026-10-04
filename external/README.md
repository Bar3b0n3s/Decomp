# Third-party code

| Directory | Upstream | Version | License |
|---|---|---|---|
| `zydis/` (submodule, includes `dependencies/zycore`) | https://github.com/zyantific/zydis | v4.1.1 (zycore v1.5.2) | MIT |
| `raw_pdb/` (submodule) | https://github.com/MolecularMatters/raw_pdb | 43cc59b | BSD-2-Clause |
| `llvm-demangle/` (vendored subset of `llvm/lib/Demangle` and `llvm/include/llvm/Demangle`) | https://github.com/llvm/llvm-project | llvmorg-23.1.2 | Apache-2.0 WITH LLVM-exception |
| `nlohmann/` (single header) | https://github.com/nlohmann/json | v3.12.0 | MIT |
| `doctest/` (single header) | https://github.com/doctest/doctest | v2.5.3 | MIT |
| `CLI11/` (headers) | https://github.com/CLIUtils/CLI11 | v2.7.2 | BSD-3-Clause |
| `imgui/` (submodule, Dear ImGui, docking branch) | https://github.com/ocornut/imgui | v1.92.9b-docking | MIT |
| `implot/` (submodule) | https://github.com/epezent/implot | v1.0 | MIT |
| `glfw/` (submodule) | https://github.com/glfw/glfw | 3.5.1 | Zlib |
| `imgui_text_edit/` (submodule, ImGuiColorTextEdit, Johan Goossens' fork) | https://github.com/goossens/ImGuiColorTextEdit | v1.92.9 | MIT |
| `fonts/jetbrains-mono/` (`JetBrainsMono-Regular.ttf`) | https://github.com/JetBrains/JetBrainsMono | v2.304 | OFL-1.1 |
| `fonts/roboto/` (`Roboto-Medium.ttf`, as shipped in Dear ImGui's `misc/fonts`) | https://github.com/googlefonts/roboto | Dear ImGui v1.92.9b-docking's copy | Apache-2.0 |
| `stb/` (`stb_image_write.h`) | https://github.com/nothings/stb | v1.16 (master `2c980bb`) | MIT or public domain |

Vendored copies are unmodified. The LLVM subset is the complete `Demangle` library (Microsoft,
Itanium, Rust and D demanglers); it has no dependency on the rest of LLVM. The only added file is
`llvm-demangle/include/llvm/Config/llvm-config.h`, a two-line stand-in for LLVM's CMake-generated header.

Zycore's OS-specific `src/API/*.c` files are not built: Zydis does not use them.

GUI libraries (`decomp-gui`): ImGui's core, `misc/cpp/imgui_stdlib.cpp`, its null backend (headless tests),
and its GLFW and OpenGL 3 backends; ImPlot without `implot_demo.cpp`; only `TextEditor.cpp` of
ImGuiColorTextEdit (not `TextDiff.cpp` and its `dtl.h`); GLFW's X11 backend on Linux and Win32 backend on
Windows. ImGuiColorTextEdit includes `imgui_internal.h`, so ImGui is pinned to the exact tag it targets.

The fonts are embedded: `src/gui/fonts/*.inc` were generated from the TTFs above with Dear ImGui's
`misc/fonts/binary_to_compressed_c.cpp -u32` (stb_compress, 32-bit words; no string literals for MSVC's
limits). The UI font is Roboto Medium because the Inter repository (v4.1) holds only the variable
`InterVariable.ttf`; its static TTFs are release downloads. `fonts/roboto/LICENSE.txt` is the Apache 2.0
text from the Roboto repository, and `stb/LICENSE` is the license block from the end of
`stb_image_write.h` (the header itself is unmodified).
