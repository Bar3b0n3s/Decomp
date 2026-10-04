// The stb_image_write implementation (external/stb), for --screenshot. Third-party code: premake5.lua
// compiles this file with warnings off. Only the *_to_func writers are used, so stdio is left out.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
